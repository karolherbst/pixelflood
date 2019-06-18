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

#include <fcntl.h>
#include <sys/mman.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/thread.h>

#include <fontconfig/fontconfig.h>

#include <GL/glcorearb.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
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
static const char IP[] = "151.217.200.48:12345";
static const uint16_t PORT = 12345;
static const uint8_t FONT_SIZE = 44;
static const float FPS_INTERVAL = 1.0; //seconds
static const bool SUPPORT_GRAY = true;
static const uint32_t BYTES = WIDTH * HEIGHT * 4;

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

static const uint8_t
hex_char_to_number_map[256] = {
	[0 ... 255] = -1,
	['0'] = 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
	['A'] = 10, 11, 12, 13, 14, 15,
	['a'] = 10, 11, 12, 13, 14, 15,
};

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
	if (SUPPORT_GRAY && unlikely(hex_char_to_number_map[color[2]] == 0xff)) {
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
quit_application(void)
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

const char text_default[] = "FPS: XXXX Clients: XXXXX Mp: XXXXXXXX kp/s: XXXXXXX Mbit/s: XXXXXXX";
const char text2_default[] = "IP:                      ";

struct draw_state {
	char *text;
	char *text2;
	TTF_Font *font;
};

static bool
draw_state_init(struct draw_state *ds)
{
	ds->text = strdup(text_default);
	ds->text2 = strdup(text2_default);

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
		return false;
	}

	ds->font = TTF_OpenFont((char *)file, FONT_SIZE);

	FcPatternDestroy(fcfont);
	FcPatternDestroy(pat);
	FcConfigDestroy(fc);
	FcFini();

	return true;
}

static void
draw_state_fini(struct draw_state *ds)
{
	TTF_CloseFont(ds->font);
	free(ds->text);
	free(ds->text2);
}

struct draw_funcs {
	SDL_RendererFlags sdl_renderer_flags;
};

struct draw_funcs draw_funcs_gl = {
	.sdl_renderer_flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC,
};

struct draw_funcs draw_funcs_sw = {
	.sdl_renderer_flags = SDL_RENDERER_SOFTWARE,
};

static int
sdl_gl_draw_loop(SDL_Window *window, struct draw_funcs *funcs)
{
	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, funcs->sdl_renderer_flags);
	if (!renderer) {
		printf("failed to create renderer\n");
		return -2;
	}

	SDL_RenderClear(renderer);

	SDL_RendererInfo info;
	if (SDL_GetRendererInfo(renderer, &info)) {
		printf("failed to get renderer info\n");
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
		SDL_DestroyRenderer(renderer);
		return -2;
	}

	SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
	SDL_ShowCursor(SDL_DISABLE);

	struct draw_state ds;
	if (!draw_state_init(&ds)) {
		printf("failed to init draw state\n");
		return -1;
	}

	pixels = malloc(BYTES);
	mtx_unlock(&px_mtx);

	SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);

	SDL_Color tcolor = { 255, 255, 255 };
	SDL_Color bgcolor = { 0, 0, 0 };
	memcpy(&ds.text2[4], IP, sizeof(IP));
	SDL_Surface *tsurface = TTF_RenderText_Shaded(ds.font, "Please stand by!                                 ", tcolor, bgcolor);
	SDL_Surface *t2surface = TTF_RenderText_Shaded(ds.font, ds.text2, tcolor, bgcolor);
	SDL_Texture *ttexture = SDL_CreateTextureFromSurface(renderer, tsurface);
	SDL_Texture *t2texture = SDL_CreateTextureFromSurface(renderer, t2surface);

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

			insert_nr_dec(&ds.text[5], fps_current, 4);
			insert_nr_dec(&ds.text[19], threads, 5);
			insert_nr_dec(&ds.text[29], pixels / 1000000, 8);
			insert_nr_dec(&ds.text[44], (pixels - px_last) / 1000, 7);
			insert_nr_dec(&ds.text[60], data_cnt / 125000, 7);

			SDL_Log("%f,%u,%"PRIu64",%"PRIu64",%"PRIu64"\n", fps, threads, pixels, pixels - px_last, data_cnt);

			px_last = pixels;
			data_cnt = 0;

			SDL_DestroyTexture(ttexture);
			SDL_FreeSurface(tsurface);
			tsurface = TTF_RenderText_Shaded(ds.font, ds.text, tcolor, bgcolor);
			ttexture = SDL_CreateTextureFromSurface(renderer, tsurface);
		}

		SDL_Rect dstrect = { 0, 0, tsurface->w, tsurface->h };
		SDL_RenderCopy(renderer, ttexture, NULL, &dstrect);
		SDL_Rect dstrect2 = { 0, tsurface->h, t2surface->w, t2surface->h };
		SDL_RenderCopy(renderer, t2texture, NULL, &dstrect2);
		SDL_RenderPresent(renderer);
	}

	SDL_DestroyTexture(ttexture);
	SDL_DestroyTexture(t2texture);
	SDL_FreeSurface(tsurface);
	SDL_FreeSurface(t2surface);

	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);

	draw_state_fini(&ds);

	TTF_Quit();
	SDL_Quit();

	mtx_lock(&px_mtx);
	free(pixels);
	mtx_unlock(&px_mtx);
	return 0;
}

const char* vs_src =
"#version 150 core\n"
"#extension GL_ARB_explicit_attrib_location : require\n"
"layout (location = 0) in vec4 aPos;\n"
"\n"
"out vec2 coords;\n"
"\n"
"void main()\n"
"{\n"
"	gl_Position = vec4(aPos.x - 1.0, aPos.y - 1.0, 0.0, 1.0);\n"
"	coords.x = aPos.x * aPos.z;\n"
"	coords.y = 1.0 - (aPos.y * aPos.w);\n"
"}\n";

const char *fs_src =
"#version 150 core\n"
"out vec4 color;\n"
"\n"
"uniform sampler2D tex;"
"\n"
"in vec2 coords;\n"
"\n"
"void main()\n"
"{\n"
"	color = texture(tex, vec2(coords.x, coords.y));\n"
"}\n";

#define GL_API_PROC(name, def) \
PFNGL##def##PROC name = SDL_GL_GetProcAddress(#name)

#define check_compile_error(shader) \
do { \
	int success; \
	char infoLog[512]; \
	glGetShaderiv(shader, GL_COMPILE_STATUS, &success); \
	if (!success) \
	{ \
		glGetShaderInfoLog(shader, 512, 0, infoLog); \
		printf("%s\n", infoLog); \
	} \
} while (false)

#define check_link_error(program) \
do { \
	int success; \
	char infoLog[512]; \
	glGetProgramiv(program, GL_LINK_STATUS, &success); \
	if (!success) { \
		glGetProgramInfoLog(program, 512, 0, infoLog); \
		printf("%s\n", infoLog); \
	} \
} while (false)

static int
gl_draw_loop(SDL_Window *window, struct draw_funcs *funcs)
{
	GL_API_PROC(glAttachShader,			ATTACHSHADER);
	GL_API_PROC(glBindBuffer,			BINDBUFFER);
	GL_API_PROC(glBindTexture, 			BINDTEXTURE);
	GL_API_PROC(glBindVertexArray,			BINDVERTEXARRAY);
	GL_API_PROC(glBufferData,			BUFFERDATA);
	GL_API_PROC(glBufferStorage,			BUFFERSTORAGE);
	GL_API_PROC(glCompileShader,			COMPILESHADER);
	GL_API_PROC(glCreateProgram,			CREATEPROGRAM);
	GL_API_PROC(glCreateShader,			CREATESHADER);
	GL_API_PROC(glDeleteBuffers,			DELETEBUFFERS);
	GL_API_PROC(glDeleteShader,             	DELETESHADER);
	GL_API_PROC(glDeleteVertexArrays,		DELETEVERTEXARRAYS);
	GL_API_PROC(glDrawElements,			DRAWELEMENTS);
	GL_API_PROC(glEnableVertexAttribArray,		ENABLEVERTEXATTRIBARRAY);
	GL_API_PROC(glFlushMappedBufferRange,		FLUSHMAPPEDBUFFERRANGE);
	GL_API_PROC(glGenBuffers,			GENBUFFERS);
	GL_API_PROC(glGenTextures, 			GENTEXTURES);
	GL_API_PROC(glGenVertexArrays,			GENVERTEXARRAYS);
	GL_API_PROC(glGetProgramiv,			GETPROGRAMIV);
	GL_API_PROC(glGetProgramInfoLog,		GETPROGRAMINFOLOG);
	GL_API_PROC(glGetShaderiv,			GETSHADERIV);
	GL_API_PROC(glGetShaderInfoLog,			GETSHADERINFOLOG);
	GL_API_PROC(glGetUniformLocation,		GETUNIFORMLOCATION);
	GL_API_PROC(glLinkProgram,			LINKPROGRAM);
	GL_API_PROC(glMapBuffer,			MAPBUFFER);
	GL_API_PROC(glMapBufferRange,			MAPBUFFERRANGE);
	GL_API_PROC(glShaderSource,			SHADERSOURCE);
	GL_API_PROC(glTexImage2D, 			TEXIMAGE2D);
	GL_API_PROC(glTexParameteri, 			TEXPARAMETERI);
	GL_API_PROC(glTexSubImage2D,			TEXSUBIMAGE2D);
	GL_API_PROC(glUniform1i,			UNIFORM1I);
	GL_API_PROC(glUnmapBuffer,			UNMAPBUFFER);
	GL_API_PROC(glUseProgram,			USEPROGRAM);
	GL_API_PROC(glVertexAttribPointer,		VERTEXATTRIBPOINTER);
	GL_API_PROC(glViewport,				VIEWPORT);

	SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
	SDL_ShowCursor(SDL_DISABLE);

	struct draw_state ds;
	if (!draw_state_init(&ds)) {
		printf("failed to init draw state\n");
		return -1;
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GLContext glcontext = SDL_GL_CreateContext(window);

	if (!glcontext) {
		SDL_GL_ResetAttributes();
		printf("failed to create GL context: %s\n", SDL_GetError());
		return -2;
	}

	bool use_gpu_buffer = true;
	if (!SDL_GL_ExtensionSupported("GL_ARB_buffer_storage")) {
		use_gpu_buffer = false;
		printf("GL_ARB_buffer_storage not supported, zero-CPU-overhead rendering not possible\n");
	}

	SDL_Color tcolor = { 255, 255, 255, 0 };
	SDL_Color bgcolor = { 0, 0, 0, 0 };
	memcpy(&ds.text2[4], IP, sizeof(IP));
	SDL_Surface *tsurface = TTF_RenderText_Shaded(ds.font, "Please stand by!                                 ", tcolor, bgcolor);
	SDL_Surface *t2surface = TTF_RenderText_Shaded(ds.font, ds.text2, tcolor, bgcolor);

	SDL_Event event;
	uint32_t fps_lasttime = SDL_GetTicks();
	uint32_t fps_current;
	uint32_t fps_frames = 0;

	uint64_t px_last = 0;

	int w, h;
	SDL_GetWindowSize(window, &w, &h);
	glViewport(0, 0, w, h);

	float vertices[] = {
		 2.0f,  2.0f, 0.5f, 0.5f,
		 2.0f,  0.0f, 0.5f, 0.5f,

		 0.0f,  0.0f, 0.5f, 0.5f,
		 0.0f,  2.0f, 0.5f, 0.5f,

		 2.0f,  0.125f, 0.5f, 8.0f,
		 2.0f,  0.0f,   0.5f, 8.0f,

		 0.0f,  0.0f,   0.5f, 8.0f,
		 0.0f,  0.125f, 0.5f, 8.0f,
	};
	unsigned int indicesp[] = {
		0, 1, 3,
		1, 2, 3,
	};
	unsigned int indicest[] = {
		4, 5, 7,
		5, 6, 7,
	};

	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vs, 1, &vs_src, NULL);
	glCompileShader(vs);
	check_compile_error(vs);

	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fs, 1, &fs_src, NULL);
	glCompileShader(fs);
	check_compile_error(fs);

	GLuint program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glLinkProgram(program);
	check_link_error(program);

	glDeleteShader(vs);
	glDeleteShader(fs);

	GLuint VBO, VAO, PBO;
	glGenVertexArrays(1, &VAO);
	glGenBuffers(1, &VBO);
	glGenBuffers(1, &PBO);

	glBindVertexArray(VAO);

	glBindBuffer(GL_ARRAY_BUFFER, VBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	// with buffer_storage we can just map GPU memory and never synchronize
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, PBO);
	if (use_gpu_buffer) {
		GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT;
		glBufferStorage(GL_PIXEL_UNPACK_BUFFER, BYTES, NULL, flags);
		pixels = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, BYTES, flags | GL_MAP_FLUSH_EXPLICIT_BIT);
		mtx_unlock(&px_mtx);
	} else {
		glBufferData(GL_PIXEL_UNPACK_BUFFER, BYTES, NULL, GL_STREAM_DRAW);
		pixels = malloc(BYTES);
		mtx_unlock(&px_mtx);
	}
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);

	GLuint ptexture;
	glGenTextures(1, &ptexture);
	glBindTexture(GL_TEXTURE_2D, ptexture);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, PBO);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, WIDTH, HEIGHT, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	GLuint ttexture;
	glGenTextures(1, &ttexture);
	glBindTexture(GL_TEXTURE_2D, ttexture);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	SDL_PixelFormat *dst_fmt = SDL_AllocFormat(SDL_PIXELFORMAT_ARGB8888);
	SDL_Surface *temp1 = SDL_ConvertSurface(tsurface, dst_fmt, 0);
	SDL_Surface *temp2 = SDL_ConvertSurface(t2surface, dst_fmt, 0);
	SDL_FreeFormat(dst_fmt);

	GLuint ccolor[WIDTH * HEIGHT];
	memset(&ccolor, 0, WIDTH * HEIGHT);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, WIDTH, temp1->h + temp2->h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &ccolor);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, temp1->w, temp1->h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, temp1->pixels);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, temp1->h, temp2->w, temp2->h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, temp2->pixels);
	SDL_FreeSurface(temp1);

	glUseProgram(program);
	glUniform1i(glGetUniformLocation(program, "tex"), 0);

	SDL_Log("fps,clients,pixels,pixels_per_second,data_kb\n");
	bool running = true;
	while (running) {
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

		fps_frames++;
		if (fps_lasttime < (SDL_GetTicks() - FPS_INTERVAL * 1000))
		{
			uint64_t pixels = nr_pixels;
			uint32_t threads = nr_clients;
			float fps = ((float)1000 * fps_frames) / (SDL_GetTicks() - fps_lasttime);
			fps_lasttime = SDL_GetTicks();
			fps_current = fps_frames;
			fps_frames = 0;

			insert_nr_dec(&ds.text[5], fps_current, 4);
			insert_nr_dec(&ds.text[19], threads, 5);
			insert_nr_dec(&ds.text[29], pixels / 1000000, 8);
			insert_nr_dec(&ds.text[44], (pixels - px_last) / 1000, 7);
			insert_nr_dec(&ds.text[60], data_cnt / 125000, 7);

			SDL_Log("%f,%u,%"PRIu64",%"PRIu64",%"PRIx64"\n", fps, threads, pixels, pixels - px_last, data_cnt);

			px_last = pixels;
			data_cnt = 0;

			SDL_FreeSurface(tsurface);
			tsurface = TTF_RenderText_Shaded(ds.font, ds.text, tcolor, bgcolor);

			SDL_PixelFormat *dst_fmt = SDL_AllocFormat(SDL_PIXELFORMAT_ARGB8888);
			temp1 = SDL_ConvertSurface(tsurface, dst_fmt, 0);
			SDL_FreeFormat(dst_fmt);

			glBindTexture(GL_TEXTURE_2D, ttexture);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, temp1->w, temp1->h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, temp1->pixels);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, temp1->h, temp2->w, temp2->h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, temp2->pixels);

			SDL_FreeSurface(temp1);
		}

		glBindTexture(GL_TEXTURE_2D, ptexture);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, PBO);
		// without buffer_storage we have to upload data to the PBO
		if (!use_gpu_buffer) {
			void *pboPixels = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
			memcpy(pboPixels, pixels, BYTES);
			glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
		} else {
			glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, BYTES);
		}
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WIDTH, HEIGHT, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, indicesp);

		glBindTexture(GL_TEXTURE_2D, ttexture);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, indicest);

		SDL_GL_SwapWindow(window);
	}

	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, PBO);
	mtx_lock(&px_mtx);
	if (use_gpu_buffer)
		glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
	else
		free(pixels);
	mtx_unlock(&px_mtx);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	glDeleteVertexArrays(1, &VAO);
	glDeleteBuffers(1, &VBO);
	glDeleteBuffers(1, &PBO);

	SDL_FreeSurface(temp2);
	SDL_FreeSurface(tsurface);
	SDL_FreeSurface(t2surface);

	SDL_GL_DeleteContext(glcontext);
	SDL_DestroyWindow(window);

	draw_state_fini(&ds);

	TTF_Quit();
	SDL_Quit();

	return 0;
}

static int
sdl_draw_loop(void *ptr)
{
	SDL_Window *window;
	SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
	TTF_Init();
	window = SDL_CreateWindow("pixelflood", 0, 0, WIDTH, HEIGHT,
		SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE);
	if (window) {
		int err = gl_draw_loop(window, &draw_funcs_gl);
		if (err != -2)
			return err;
		printf("failed to create OpenGL context. Falling back to hw SDL rendering\n");

		err = sdl_gl_draw_loop(window, &draw_funcs_gl);
		if (err != -2)
			return err;

		printf("failed to create hw SDL context. Falling back to sw SDL rendering\n");
		SDL_DestroyWindow(window);
	}

	window = SDL_CreateWindow("pixelflood", 0, 0, WIDTH, HEIGHT,
		SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE);
	if (window)
		return sdl_gl_draw_loop(window, &draw_funcs_sw);

	mtx_unlock(&px_mtx);
	printf("failed to create SDL window\n");
	exit(-1);
	return -1;
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

static inline uint8_t *
parse_line(uint8_t *line, struct client_data *client, uint16_t *l_nr_pixels)
{
	if (likely(line[0] == 'P')) {
		line = &line[3];
		int x = read_nr_dec(&line);
		line = &line[1];
		int y = read_nr_dec(&line);
		if (unlikely(*line == '\n')) {
			char out[28];
			uint32_t data = pixels[x + y * WIDTH];
			// convert from argb to rgba
			size_t l = sprintf(out, "PX %i %i %x\n", x, y, (data >> 8) | (data << 24));
			send(client->c, out, l, 0);
		} else {
			line = &line[1];
			uint32_t argb = read_nr_hex(&line);
			updatePxARGB(x, y, argb);
			++(*l_nr_pixels);
		}
	} else if (line[0] == 'S') {
		char out[20];
		size_t l = sprintf(out, "SIZE %i %i\n", WIDTH, HEIGHT);
		send(client->c, out, l, 0);
		while (line[0] != '\n')
			line = &line[1];
	} else {
		while (line[0] != '\n')
			line = &line[1];
	}

	return line;
}

static inline uint8_t *
parse_line_ex(uint8_t *buffer, struct client_data *client, uint16_t *l_nr_pixels)
{
	bool save = false;
	uint8_t *line;
	if (client->len) {
		// we cheat a little here
		line = buffer;
		int i = 0;
		while (line[i] != '\n')
			++i;

		memcpy(&client->stored_cmd[client->len], buffer, i + 1);
		client->len = 0;
		line = client->stored_cmd;
		buffer = &buffer[i + 1];
	} else {
		line = buffer;
		save = true;
	}

	line = parse_line(line, client, l_nr_pixels);

	if (save)
		buffer = &line[1];
	return buffer;
}

static inline uint8_t *
parse_line_simple(uint8_t *buffer, struct client_data *client, uint16_t *l_nr_pixels)
{
	return &parse_line(buffer, client, l_nr_pixels)[1];
}

static void
on_read(struct bufferevent *bev, void *data)
{
	struct client_data *client = (struct client_data *)data;

	struct evbuffer_iovec v;
	struct evbuffer *buf = bufferevent_get_input(bev);
	evbuffer_peek(buf, -1, NULL, &v, 1);
	uint8_t *sbuffer = v.iov_base;
	uint8_t *buffer = v.iov_base;
	uint16_t l_nr_pixels = 0;

	data_cnt += v.iov_len;

	int i;
	int until = v.iov_len - 1;
	for (i = until; i >= 0; --i)
		if (buffer[i] == '\n')
			break;

	if (i == -1) {
		memcpy(&client->stored_cmd[client->len], buffer, v.iov_len);
		client->len += v.iov_len;
		evbuffer_drain(buf, v.iov_len);
		return;
	}

	sbuffer = &sbuffer[i + 1];

	// first command might need to be extended
	buffer = parse_line_ex(buffer, client, &l_nr_pixels);
	while (buffer != sbuffer)
		buffer = parse_line_simple(buffer, client, &l_nr_pixels);

	if (i != until) {
		// store the data for the next iteration:
		client->len = v.iov_len - (i + 1);
		memcpy(client->stored_cmd, buffer, client->len);
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

int
server(void)
{
	pthread_setname_np(thrd_current(), "pixelflood main");

	thrd_t dsp_thread;

	mtx_init(&px_mtx, mtx_plain);
	mtx_lock(&px_mtx);

	if (thrd_create(&dsp_thread, sdl_draw_loop, NULL)) {
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
	mtx_unlock(&px_mtx);
	thrd_join(dsp_thread, NULL);
	mtx_destroy(&px_mtx);
	free(thread_data);

	return EXIT_SUCCESS;
}

int
fuzzing(const char *file)
{
	int f = open(file, O_RDONLY, 0);
	if (f == -1)
		return EXIT_FAILURE;

	struct stat st;
	if (stat(file, &st))
		return EXIT_FAILURE;

	uint8_t *line = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, f, 0);
	uint16_t px;
	struct client_data client = {
		.c = 0,
		.len = 0,
	};

	pixels = malloc(BYTES);
	parse_line_simple(line, &client, &px);
	free(pixels);

	return EXIT_SUCCESS;
}


int
main(int argc, char const* const* argv)
{
	if (argc == 1)
		return server();

	if (!strcmp(argv[1], "fuzz") && argc == 3)
		return fuzzing(argv[2]);
}
