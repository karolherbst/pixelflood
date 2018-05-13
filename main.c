#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define __USE_GNU 1
#include <pthread.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
//#define likely(x) (x)
//#define unlikely(x) (x)

static bool running;
static uint32_t *pixels;
static atomic_uint_fast64_t nr_pixels;
static atomic_uint_fast64_t nr_threads;

static const uint32_t NET_BUFFER = 15000;
static const uint32_t WIDTH = 1920;
static const uint32_t HEIGHT = 1080;
static const float FPS_INTERVAL = 1.0; //seconds

static const char WHITESPACE[] = " \t";
static const char NUMBERS[] = "0123456789";
static const char NUMBERSHEX[] = "0123456789abcdefABCDEF";

static void
updatePx(int x, int y, int r, int g, int b, int a)
{
	if (unlikely(x >= WIDTH || y >= HEIGHT))
		return;

	uint32_t data = b | (g << 8) | (r << 16) | (a << 24);
	pixels[x + y * WIDTH] = data;
	atomic_fetch_add(&nr_pixels, 1);
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

	for (;;)
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
	bool done = false;
	while (!done)
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

static void*
draw_loop(void *ptr)
{
	char text[] = "FPS: XXXX; Connections: XXXXX; Megapixels: XXXXXXXXXXXXXX; p/s: XXXXXXXXXX";

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

	while (running) {
		uint32_t pitch = WIDTH * sizeof(*pixels);

		if (SDL_PollEvent(&event)) {
			switch (event.type) {
			case SDL_KEYDOWN: {
				switch (event.key.keysym.sym) {
				case SDLK_q:
					running = false;
				}
				break;
			}
			case SDL_QUIT:
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
			uint64_t pixels = atomic_load(&nr_pixels);
			uint64_t threads = atomic_load(&nr_threads);
			fps_lasttime = SDL_GetTicks();
			fps_current = fps_frames;
			fps_frames = 0;

			insert_nr_dec(&text[5], fps_current, 4);
			insert_nr_dec(&text[24], threads, 5);
			insert_nr_dec(&text[43], pixels, 14);
			insert_nr_dec(&text[64], pixels - px_last, 10);

			px_last = pixels;

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
}

struct thread_data {
	int c;
};

static void*
read_input(void *data)
{
	char buffer[NET_BUFFER + 50];

	atomic_fetch_add(&nr_threads, 1);

	struct thread_data *td = (struct thread_data *)data;
	char *line;

	while (running) {
		ssize_t last_pos = 0;
		ssize_t r = recv(td->c, buffer, NET_BUFFER, 0);

		if (unlikely(r == -1))
			goto out;

		if (unlikely(r == 0))
			goto out;

		for (int i = 0; i <= NET_BUFFER; ++i) {
			if (unlikely(buffer[i] == EOF))
				goto out;

			if (i == NET_BUFFER && buffer[i] != '\n') {
				while (i < NET_BUFFER + 50 && 1 == recv(td->c, &buffer[i], 1, 0) && buffer[i] != '\n')
					++i;
			}

			if (buffer[i] == '\n') {
				line = &buffer[last_pos];
				last_pos = i + 1;

				if (unlikely(line[0] == 'S')) {
					char out[100];
					size_t l = sprintf(out, "SIZE %i %i\n", WIDTH, HEIGHT);
					send(td->c, out, l, 0);
				} else if (likely(line[0] == 'P' && line[1] == 'X')) {
					char *l = &line[2];
					l = &l[1];
					int x = read_nr_dec(&l);
					l = &l[1];
					int y = read_nr_dec(&l);
					if (unlikely(*l == '\n')) {
						char out[100];
						uint32_t data;
						if (likely(pixels != NULL))
							data = pixels[x + y * WIDTH];
						else
							data = 0;
						size_t l = sprintf(out, "PX %i %i %f\n", x, y, 0);
						send(td->c, out, l, 0);
					} else {
						l = &l[1];
						uint32_t c = read_nr_hex(&l);
						updatePx(x, y, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, 0);
					}
				}
			}
		}
	}

out:
	atomic_fetch_add(&nr_threads, -1);
	close(td->c);
	free(td);
	return NULL;
}

int main()
{
	pthread_t dsp_thread;
	struct thread_data data[8];
	pthread_t read_thread;

	running = true;
	atomic_init(&nr_pixels, 0);
	atomic_init(&nr_threads, 0);
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

	while (running) {
		struct thread_data *data = malloc(sizeof(*data));
		data->c = accept(s, (struct sockaddr*)NULL, NULL);

		pthread_create(&read_thread, NULL, read_input, data);
		pthread_setname_np(read_thread, "pixelflood data");
	}

	pthread_join(dsp_thread, NULL);

	free(pixels);
	return EXIT_SUCCESS;
}
