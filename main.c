#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define __USE_GNU 1
#include <pthread.h>
#include <threads.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/thread.h>

#include <fontconfig/fontconfig.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

// for performance we don't ever lock the pixels buffer, because after a while
// it doesn't matter anyway
static uint32_t *pixels;
mtx_t px_mtx;
/* strictly speaking the number of drawn pixels should be an atomic, but it
 * has a too big impact on performance and having a correct value doesn't
 * matter anyway
 */
static _Atomic uint64_t nr_pixels;
static _Atomic uint64_t data_cnt = 0;
static _Atomic uint32_t nr_clients;

static const uint32_t WIDTH = 1920;
static const uint32_t HEIGHT = 1080;
static const uint32_t THREADS = 8;
static const uint16_t PORT = 12345;
static const float FPS_INTERVAL = 1.0; //seconds

static struct event_base *evbase;

// we have to load balance a little, so keep information about each thread
struct ThreadData {
	thrd_t t;
	struct event_base *evbase;
} *thread_data;

static void
updatePxARGB(uint_fast16_t x, uint_fast16_t y, uint32_t argb)
{
	if (unlikely(x >= WIDTH || y >= HEIGHT))
		return;
	pixels[x + y * WIDTH] = argb;
}

static uint8_t
hex_char_to_number_map[256];

static void
init_char_to_number_map() {
	memset(hex_char_to_number_map, 0xff, 256);

	for (int i = 0; i <= 0x9; ++i)
		hex_char_to_number_map['0' + i] = i;

	for (int i = 0; i < 6; ++i) {
		hex_char_to_number_map['a' + i] = 0xa + i;
		hex_char_to_number_map['A' + i] = 0xa + i;
	}
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

static inline uint32_t
read_nr_dec(uint8_t **buf)
{
	uint32_t result = 0;

	while (true)
	{
		uint8_t c = **buf;
		uint8_t v = c - '0';
		if (v > 9)
			break;
		result *= 10;
		result += v;
		*buf = &(*buf)[1];
	}

	return result;
}

// we only get 2, 6 or 8 chars
static inline uint32_t
read_nr_hex(uint8_t **buf)
{
	uint8_t *color = *buf;

	// gray
	if (hex_char_to_number_map[color[2]] == 0xff) {
		return
			hex_char_to_number_map[color[0]] << 20 |
			hex_char_to_number_map[color[1]] << 16 |
			hex_char_to_number_map[color[0]] << 12 |
			hex_char_to_number_map[color[1]] <<  8 |
			hex_char_to_number_map[color[0]] <<  4 |
			hex_char_to_number_map[color[1]] <<  0;
	}

	uint32_t result =
		hex_char_to_number_map[color[0]] << 20 |
		hex_char_to_number_map[color[1]] << 16 |
		hex_char_to_number_map[color[2]] << 12 |
		hex_char_to_number_map[color[3]] <<  8 |
		hex_char_to_number_map[color[4]] <<  4 |
		hex_char_to_number_map[color[5]] <<  0;

	uint8_t al = hex_char_to_number_map[color[6]];

	*buf = &(*buf)[6];
	if (al == 0xff)
		return result | 0xff000000;

	result |=
		hex_char_to_number_map[color[7]] << 24 |
		al << 28;
	*buf = &(*buf)[2];
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
	for (int i = 0; i < THREADS; ++i)
		event_base_loopexit(thread_data[i].evbase, &timeout);

	event_base_loopbreak(evbase);
	for (int i = 0; i < THREADS; ++i)
		event_base_loopbreak(thread_data[i].evbase);
}

static int
draw_loop(void *ptr)
{
	pixels = malloc(sizeof(*pixels) * WIDTH * HEIGHT);
	mtx_unlock(&px_mtx);

	char text[] = "FPS: XXXX Clients: XXXXX Mp: XXXXXXXX kp/s: XXXXXXX Mbit/s: XXXXXXX";

	SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);

	SDL_Window *window = SDL_CreateWindow("pixelflood", 0, 0, WIDTH, HEIGHT,
		SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE);
	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	SDL_RenderClear(renderer);

	SDL_RendererInfo info;
	if (SDL_GetRendererInfo(renderer, &info)) {
		printf("failed to get rendered info\n");
		exit(-1);
		return -1;
	}

	bool found_format = false;
	for (int i = 0; i < info.num_texture_formats; ++i) {
		if (info.texture_formats[i] == SDL_PIXELFORMAT_ARGB8888)
			found_format = true;
	}
	if (!found_format) {
		printf("couldn't find supported texture format\n");
		exit(-1);
		return -1;
	}

	SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
	SDL_ShowCursor(SDL_DISABLE);

	SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);

	FcConfig* fc = FcInitLoadConfigAndFonts();
	FcPattern* pat = FcNameParse((FcChar8 *)"FreeMono");
	FcConfigSubstitute(fc, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);

	FcResult result;
	FcPattern* fcfont = FcFontMatch(fc, pat, &result);
	FcChar8* file = NULL;
	if (fcfont)
		FcPatternGetString(fcfont, FC_FILE, 0, &file);
	if (!file) {
		printf("Couldn't find FreeMono.ttf\n");
		exit(-1);
		return -1;
	}

	TTF_Init();
	TTF_Font *font = TTF_OpenFont((char *)file, HEIGHT / 12);
	SDL_Color color = { 255, 255, 255 };
	SDL_Rect dstrect = { 0, 0, WIDTH, HEIGHT / 20 };
	SDL_Surface *tsurface = TTF_RenderText_Solid(font, "Please stand by!                                 ", color);
	SDL_Texture *ttexture = SDL_CreateTextureFromSurface(renderer, tsurface);

	FcPatternDestroy(fcfont);
	FcPatternDestroy(pat);
	FcConfigDestroy(fc);
	FcFini();

	SDL_Event event;
	uint32_t fps_lasttime = SDL_GetTicks();
	uint32_t fps_current;
	uint32_t fps_frames = 0;

	uint64_t px_last = 0;

	SDL_Log("fps,clients,pixels,pixels_per_second,data_kb\n");
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
		SDL_RenderCopy(renderer, texture, NULL, NULL);

		fps_frames++;
		if (fps_lasttime < (SDL_GetTicks() - FPS_INTERVAL * 1000))
		{
			uint64_t pixels = nr_pixels;
			uint32_t threads = nr_clients;
			float fps = ((float)1000 * fps_frames) / (SDL_GetTicks() - fps_lasttime);
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
			SDL_Log("%f,%u,%"PRIu64",%"PRIu64",%"PRIu64"\n", fps, threads, pixels, pixels - px_last, data_cnt);
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
	return 0;
}

struct client_data {
	int c;
	uint8_t stored_cmd[50];
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

static inline void
parse_line(uint8_t *buffer, struct client_data *client, int i, ssize_t *last_pos, uint32_t *l_nr_pixels)
{
	uint8_t *line;
	if (unlikely(client->len)) {
		// we cheat a little here
		memcpy(&client->stored_cmd[client->len], buffer, i + 1);
		client->len = 0;
		line = client->stored_cmd;
	} else
		line = &buffer[*last_pos];
	*last_pos = i + 1;

	if (likely(line[0] == 'P' && line[1] == 'X')) {
		uint8_t *l = &line[2];
		l = &l[1];
		int x = read_nr_dec(&l);
		l = &l[1];
		int y = read_nr_dec(&l);
		if (unlikely(*l == '\n')) {
			char out[28];
			uint32_t data = pixels[x + y * WIDTH];
			// convert from argb to rgba
			size_t l = sprintf(out, "PX %i %i %x\n", x, y, (data >> 8) | (data << 24));
			send(client->c, out, l, 0);
		} else {
			l = &l[1];
			uint32_t argb = read_nr_hex(&l);
			updatePxARGB(x, y, argb);
			++(*l_nr_pixels);
		}
	} else if (line[0] == 'S') {
		char out[20];
		size_t l = sprintf(out, "SIZE %i %i\n", WIDTH, HEIGHT);
		send(client->c, out, l, 0);
	}
}

static void
on_read(struct bufferevent *bev, void *data)
{
	struct client_data *client = (struct client_data *)data;

	struct evbuffer_iovec v;
	struct evbuffer *buf = bufferevent_get_input(bev);
	evbuffer_peek(buf, -1, NULL, &v, 1);
	uint8_t *buffer = v.iov_base;
	uint32_t l_nr_pixels = 0;

	ssize_t last_pos = 0;
	data_cnt += v.iov_len;

	int i;
	int until = v.iov_len - 1;
	for (i = 0; likely(i < until); ++i) {
		if (buffer[i] == '\n')
			parse_line(buffer, client, i, &last_pos, &l_nr_pixels);
	}

	++i;
	if (buffer[i] == '\n')
		parse_line(buffer, client, i, &last_pos, &l_nr_pixels);
	else {
		// store the data for the next iteration:
		client->len = v.iov_len - last_pos;
		memcpy(client->stored_cmd, &buffer[last_pos], client->len);
	}

	nr_pixels += l_nr_pixels;
	evbuffer_drain(buf, v.iov_len);
}

static void
on_accept(struct evconnlistener *listener, evutil_socket_t s, struct sockaddr *address, int socklen, void *ctx)
{
	struct client_data *client = malloc(sizeof(*client));
	client->len = 0;

	// that scheduling is crap..
	struct event_base *evbase = thread_data[nr_clients++ % THREADS].evbase;

        struct bufferevent *bev = bufferevent_socket_new(evbase, s, 0);
        bufferevent_setcb(bev, on_read, NULL, on_error, client);
        bufferevent_enable(bev, EV_READ | EV_PERSIST);
}

int
read_thread(void *data)
{
	struct ThreadData *td = data;

	// each thread has its own event loop
	// pseudo event so that the thread never exit
	struct event *ev = event_new(td->evbase, -1, EV_PERSIST | EV_WRITE, NULL, NULL);
	event_add(ev, NULL);

	event_base_dispatch(td->evbase);

	event_free(ev);
	event_base_free(td->evbase);
	thrd_exit(0);
	return 0;
}

int main()
{
	pthread_setname_np(thrd_current(), "pixelflood main");

	thrd_t dsp_thread;

	init_char_to_number_map();
	mtx_init(&px_mtx, mtx_plain);
	mtx_lock(&px_mtx);

	if (thrd_create(&dsp_thread, draw_loop, NULL)) {
		printf("failed to create display thread!\n");
		return EXIT_FAILURE;
	}
	pthread_setname_np(dsp_thread, "pixelflood disp");

	struct sockaddr_in serv_addr = { 0 };

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(PORT);

	evthread_use_pthreads();

	mtx_lock(&px_mtx);
	mtx_unlock(&px_mtx);
	mtx_destroy(&px_mtx);
	// initialize threads
	thread_data = malloc(sizeof(struct ThreadData) * THREADS);
	for (int i = 0; i < THREADS; ++i) {
		struct ThreadData *td = &thread_data[i];
		td->evbase = event_base_new();
		thrd_create(&td->t, read_thread, td);
		pthread_setname_np(td->t, "pixelflood net");
	}

	evbase = event_base_new();
	evconnlistener_new_bind(evbase, on_accept, NULL, LEV_OPT_CLOSE_ON_FREE, -1, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

	event_base_dispatch(evbase);

	for (int i = 0; i < THREADS; ++i) {
		thrd_join(thread_data[i].t, NULL);
	}
	thrd_join(dsp_thread, NULL);
	free(thread_data);
	free(pixels);

	return EXIT_SUCCESS;
}
