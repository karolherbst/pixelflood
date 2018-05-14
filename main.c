#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define __USE_GNU 1
#include <pthread.h>

#include <arpa/inet.h>

#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/event_struct.h>
#include <fcntl.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

// for performance we don't ever lock the pixels buffer, because after a while
// it doesn't matter anyway
static uint32_t *pixels;
/* strictly speaking the number of drawn pixels should be an atomic, but it
 * has a too big impact on performance and having a correct value doesn't
 * matter anyway
 */
static uint64_t nr_pixels;
static uint64_t data_cnt = 0;
static _Atomic uint32_t nr_clients;

// 1 << 14 seems to be a good enough value
// lower value decreases displaying latency, but reducing overall throughput
static const uint32_t NET_BUFFER_SIZE = 1 << 14;
static const uint32_t WIDTH = 1920;
static const uint32_t HEIGHT = 1080;
static const float FPS_INTERVAL = 1.0; //seconds

static struct event_base *evbase;

static int
setnonblock(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return flags;
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0)
		return -1;

	return 0;
}

static void
updatePx(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	if (unlikely(x >= WIDTH || y >= HEIGHT))
		return;

	uint32_t data = b | (g << 8) | (r << 16) | (a << 24);
	pixels[x + y * WIDTH] = data;
	++nr_pixels;
}

static void
insert_nr_dec(char *buf, uint64_t value, uint16_t length)
{
	while (length > 0)
	{
		buf[--length] = '0' + value % 0xa;
		value /= 0xa;
	}
}

static uint64_t
read_nr_dec(char **buf)
{
	uint64_t result = 0;

	while (true)
	{
		char c = **buf;
		if (unlikely(c < '0' || c > '9'))
			return result;

		result *= 10;
		result += c - '0';
		*buf = &(*buf)[1];
	}

	return result;
}

static uint64_t
read_nr_hex(char **buf)
{
	uint64_t result = 0;
	while (true)
	{
		char c = **buf;
		if (c >= '0' && c<= '9') {
			result *= 0x10;
			result += c - '0';
		} else if (c >= 'a' && c <= 'f') {
			result *= 0x10;
			result += c - 'a' + 0xa;
		} else if (c >= 'A' && c <= 'F') {
			result *= 0x10;
			result += c - 'A' + 0xa;
		} else
			break;
		*buf = &(*buf)[1];
	}

	return result;
}

static void
quit_application()
{
	struct timeval timeout = {
		.tv_sec = 1,
		.tv_usec = 0,
	};
	event_base_loopexit(evbase, &timeout);
}

static void*
draw_loop(void *ptr)
{
	char text[] = "FPS: XXXX Clients: XXXXX Mp: XXXXXXXX kp/s: XXXXXXX Mbit/s: XXXXXXX";

	SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
	SDL_Window *window = SDL_CreateWindow("pixelflood", 0, 0, WIDTH, HEIGHT,
		SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE);
	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);

	TTF_Init();
	TTF_Font *font = TTF_OpenFont("/usr/share/fonts/gnu-free/FreeMono.ttf", 24);
	SDL_Color color = { 255, 255, 255 };
	SDL_Rect dstrect = { 0, 0, WIDTH, 50 };
	SDL_Surface *tsurface = TTF_RenderText_Solid(font, "Please stand by!                                 ", color);
	SDL_Texture *ttexture = SDL_CreateTextureFromSurface(renderer, tsurface);

	SDL_Event event;
	uint32_t fps_lasttime = SDL_GetTicks();
	uint32_t fps_current;
	uint32_t fps_frames = 0;

	uint64_t px_last = 0;

	bool running = true;
	while (running) {
		uint32_t pitch = WIDTH * sizeof(*pixels);

		if (SDL_PollEvent(&event)) {
			switch (event.type) {
			case SDL_KEYDOWN: {
				switch (event.key.keysym.sym) {
				case SDLK_q:
					quit_application();
					running = false;
				}
				break;
			}
			case SDL_QUIT:
				quit_application();
				running = false;
				break;
			}
		}

		SDL_UpdateTexture(texture, NULL, pixels, pitch);
		SDL_RenderClear(renderer);
		SDL_RenderCopy(renderer, texture, NULL, NULL);

		fps_frames++;
		if (fps_lasttime < (SDL_GetTicks() - FPS_INTERVAL * 1000))
		{
			uint64_t pixels = nr_pixels;
			uint64_t threads = nr_clients;
			fps_lasttime = SDL_GetTicks();
			fps_current = fps_frames;
			fps_frames = 0;

			insert_nr_dec(&text[5], fps_current, 4);
			insert_nr_dec(&text[19], threads, 5);
			insert_nr_dec(&text[29], pixels / 1000000, 8);
			insert_nr_dec(&text[44], (pixels - px_last) / 1000, 7);
			insert_nr_dec(&text[60], data_cnt / 125000, 7);

			px_last = pixels;
			data_cnt = 0;

			SDL_DestroyTexture(ttexture);
			SDL_FreeSurface(tsurface);
			tsurface = TTF_RenderText_Solid(font, (const char*)&text, color);
			ttexture = SDL_CreateTextureFromSurface(renderer, tsurface);
		}

		SDL_RenderCopy(renderer, ttexture, NULL, &dstrect);
		SDL_RenderPresent(renderer);
	}

	SDL_DestroyTexture(ttexture);
	SDL_FreeSurface(tsurface);
	TTF_CloseFont(font);
	TTF_Quit();

	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return NULL;
}

struct client_data {
	int c;
	struct bufferevent *bev;
	char stored_cmd[50];
	uint8_t len;
};

static void
on_error(struct bufferevent *bev, short ev, void *data)
{
	struct client_data *client = (struct client_data *)data;

	bufferevent_free(bev);
	close(client->c);
	free(client);
	--nr_clients;
}

static void
on_read(struct bufferevent *bev, void *data)
{
	struct client_data *client = (struct client_data *)data;
	char buffer[NET_BUFFER_SIZE + 50];

	size_t offset = client->len;
	client->len = 0;
	memcpy(buffer, client->stored_cmd, offset);

	ssize_t last_pos = 0;
	size_t r = bufferevent_read(bev, &buffer[offset], NET_BUFFER_SIZE);
	data_cnt += r;

	for (int i = offset; i < (r + offset); ++i) {
		if (i == r + offset - 1 && buffer[i] != '\n') {
			// store the data for the next iteration:
			client->len = r + offset - last_pos;
			memcpy(client->stored_cmd, &buffer[last_pos], client->len);
			return;
		}

		if (buffer[i] == '\n') {
			char *line = &buffer[last_pos];
			last_pos = i + 1;

			if (likely(line[0] == 'P' && line[1] == 'X')) {
				char *l = &line[2];
				l = &l[1];
				int x = read_nr_dec(&l);
				l = &l[1];
				int y = read_nr_dec(&l);
				if (unlikely(*l == '\n')) {
					char out[28];
					uint32_t data;
					if (likely(pixels != NULL))
						data = pixels[x + y * WIDTH];
					else
						data = 0;
					size_t l = sprintf(out, "PX %i %i %x\n", x, y, data);
					send(client->c, out, l, 0);
				} else {
					l = &l[1];
					uint32_t c = read_nr_hex(&l);
					updatePx(x, y, c >> 16, c >> 8, c, 0);
				}
			} else if (likely(line[0] == 'S')) {
				char out[20];
				size_t l = sprintf(out, "SIZE %i %i\n", WIDTH, HEIGHT);
				send(client->c, out, l, 0);
			}
		}
	}
}

static void
on_accept(int s, short ev, void *data)
{
	struct client_data *client = malloc(sizeof(*client));
	client->c = accept(s, (struct sockaddr*)NULL, NULL);
	client->len = 0;

	++nr_clients;

	setnonblock(client->c);
	client->bev = bufferevent_socket_new(evbase, client->c, 0);
	bufferevent_setcb(client->bev, on_read, NULL, on_error, client);
	// we never want to buffer data beyond what we can process
	bufferevent_setwatermark(client->bev, EV_READ, 15, NET_BUFFER_SIZE);
	bufferevent_enable(client->bev, EV_READ);
}

int main()
{
	pthread_t dsp_thread;

	pixels = malloc(sizeof(*pixels) * WIDTH * HEIGHT);

	if (pthread_create(&dsp_thread, NULL, draw_loop, NULL)) {
		printf("failed to create display thread!\n");
		return EXIT_FAILURE;
	}
	pthread_setname_np(dsp_thread, "pixelflood disp");

	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in serv_addr = { 0 };

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(12345);

	bind(s, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
	listen(s, 1000);

	int reuseaddr = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr));
	setnonblock(s);

	evbase = event_base_new();

	struct event ev_accept;
	event_assign(&ev_accept, evbase, s, EV_READ | EV_PERSIST, on_accept, NULL);
	event_add(&ev_accept, NULL);

	event_base_dispatch(evbase);
	pthread_join(dsp_thread, NULL);
	free(pixels);

	return EXIT_SUCCESS;
}
