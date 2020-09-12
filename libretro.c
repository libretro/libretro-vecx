#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "osint.h"
#include "vecx.h"
#include "e8910.h"
#include "e6809.h"
#include "libretro.h"
#include "glsym/glsym.h"
#include "libretro_core_options.h"
#include "dots.h"

#define STANDARD_BIOS

#ifdef STANDARD_BIOS
#include "bios/system.h"
#else
#ifdef FAST_BIOS
#include "bios/fast.h"
#else
#include "bios/skip.h"
#endif
#endif

static int WIDTH = 330;
static int HEIGHT = 410;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#if defined(HAVE_PSGL)
#define RARCH_GL_FRAMEBUFFER GL_FRAMEBUFFER_OES
#define RARCH_GL_FRAMEBUFFER_COMPLETE GL_FRAMEBUFFER_COMPLETE_OES
#define RARCH_GL_COLOR_ATTACHMENT0 GL_COLOR_ATTACHMENT0_EXT
#elif defined(OSX_PPC)
#define RARCH_GL_FRAMEBUFFER GL_FRAMEBUFFER_EXT
#define RARCH_GL_FRAMEBUFFER_COMPLETE GL_FRAMEBUFFER_COMPLETE_EXT
#define RARCH_GL_COLOR_ATTACHMENT0 GL_COLOR_ATTACHMENT0_EXT
#else
#define RARCH_GL_FRAMEBUFFER GL_FRAMEBUFFER
#define RARCH_GL_FRAMEBUFFER_COMPLETE GL_FRAMEBUFFER_COMPLETE
#define RARCH_GL_COLOR_ATTACHMENT0 GL_COLOR_ATTACHMENT0
#endif

#ifdef _3DS
#define BUFSZ 135300
#else
#define BUFSZ 2164800
#endif

static bool usingHWContext = false;
static struct retro_hw_render_callback hw_render;
static retro_video_refresh_t video_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_t audio_cb;

static unsigned char point_size;
static unsigned short framebuffer[BUFSZ];

static GLuint ProgramID;
static GLuint mvpMatrixLocation;
static GLuint positionAttribLocation;
static GLuint offsetAttribLocation;
static GLuint colourAttribLocation;
static GLuint packedTexCoordsAttribLocation;
static GLuint textureLocation;
static GLuint scaleLocation;
static GLuint brightnessLocation;
static GLuint DotTextureID;
static GLuint GlowTextureID;
static GLuint vbo;
GLfloat mvpMatrix[16];

static const float dotScale = 1.0f;
static float lineWidth = 75.0f;
static float lineBrightness = 216.0f;
static float glowWidthMultiplier = 8.0f;
static float maxAlpha = 0.2f;
static const float glowBrightness = 200.0f;

typedef struct {
    float x;
    float y;
} POINT;

typedef struct {
    uint32_t pos;
    union {
        uint32_t rest;
        struct {
            uint16_t offsets;
            GLbyte colour;
            GLubyte packedTexCoords;
        };
    };
} GLVERTEX;
#define MAX_VECTORS 50000
static GLVERTEX verticies[MAX_VECTORS * 18];

extern unsigned char vecx_ram[1024];

/* Empty stubs */
void retro_set_controller_port_device(unsigned port, unsigned device){}
void retro_cheat_reset(void){}
void retro_cheat_set(unsigned index, bool enabled, const char *code){}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {}
unsigned retro_get_region(void){ return RETRO_REGION_PAL; }
unsigned retro_api_version(void){ return RETRO_API_VERSION; }
bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info){ return false; }

void retro_deinit(void)
{
}

void *retro_get_memory_data(unsigned id)
{ 
   if ( id == RETRO_MEMORY_SYSTEM_RAM )
      return vecx_ram;
   return NULL; 
}
size_t retro_get_memory_size(unsigned id)
{
   if ( id == RETRO_MEMORY_SYSTEM_RAM )
      return 1024;
   return 0; 
}

/* Emulator states */
extern unsigned snd_regs[16];

/* setters */
void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
   libretro_set_core_options(environ_cb);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->library_name = "VecX";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
	info->library_version = "1.2" GIT_VERSION;
	info->need_fullpath = false;
	info->valid_extensions = "bin|vec";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof(*info));
	info->timing.fps            = 50.0;
	info->timing.sample_rate    = 44100;
	info->geometry.base_width   = WIDTH;
	info->geometry.base_height  = HEIGHT;
#ifdef _3DS
	info->geometry.max_width    = 330;
	info->geometry.max_height   = 410;
#else
	info->geometry.max_width    = 2048;
	info->geometry.max_height   = 2048;
#endif
		
	info->geometry.aspect_ratio = 33.0 / 41.0;
}

void CreateImage(uint32_t width, uint32_t height, const uint8_t *data, GLuint *textureId)
{
    GLenum err;
    glGenTextures(1, textureId);
    if ((err = glGetError()))
    {
        fprintf(stderr, "Error generating texture: %x\n", err);
    }
    glBindTexture(GL_TEXTURE_2D, *textureId);
    if ((err = glGetError()))
    {
        fprintf(stderr, "Error binding texture: %x\n", err);
    }
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
    if ((err = glGetError()))
    {
        fprintf(stderr, "Error loading texture: %x\n", err);
    }
}

void MakeMVPMatrix(float mvpMatrix[16], float left, float bottom, float right, float top)
{
    static const float near = -1.0f;
    static const float far  = 1.0f;
    
    for (int i=0; i<16; i++)
        mvpMatrix[i] = 0;
    
    mvpMatrix[0] = 2.0f/(right-left);
    mvpMatrix[3] = -(right+left)/(right-left);
    mvpMatrix[5] = 2.0f/(top-bottom);
    mvpMatrix[7] = -(top+bottom)/(top-bottom);
    mvpMatrix[10] = -2.0f/(far-near);
    mvpMatrix[11] = -(far+near)/(far-near);
    mvpMatrix[15] = 1.0f;
}


static void compile_program(void)
{
    const GLchar *vertexShaderSource[] = {
                                          "attribute vec2 position;\n"
                                          "attribute vec2 offset;\n"
                                          "attribute float colour;\n"
                                          "attribute float packedTexCoords;\n"
                                          "uniform mat4  mvpMatrix;\n"
                                          "uniform float scale;\n"
                                          "uniform float brightness;\n"

                                          "varying float fragColour;\n"
                                          "varying vec2 fragTexCoords;\n"
                                          
                                          " void main()\n"
                                          "{\n"
                                          "   vec2 pos = position + (offset / 64.0) * scale;\n"
                                          "   fragColour = colour * brightness / (127.0 * 255.0);\n" 
                                          "   float tx = floor(packedTexCoords * 0.0625);\n" // RPI gets upset if we divide by 16 so multiply by 1/16 instead.
                                          "   float ty = packedTexCoords - tx * 16.0;\n"
                                          "   fragTexCoords = vec2(tx, ty) / 2.0;\n"
                                          "   gl_Position = vec4(pos, 0.0, 1.0) * mvpMatrix;\n"
                                          "}\n"
                                          };
    
    const char *fragmentShaderSource[] = {
                                          "#ifdef GL_ES\n"
                                          "precision mediump float;\n"
                                          "#endif\n"
                                          "uniform sampler2D texture;\n"

                                          "varying float fragColour;\n"
                                          "varying vec2 fragTexCoords;\n"
                                          
                                          "void main()\n"
                                          "{\n"
                                          "   vec4 colour = texture2D(texture, fragTexCoords).rgbr;\n"
                                          "   colour *= fragColour;\n"
                                          "   gl_FragColor = colour;\n"
                                          "}\n"
                                          };


    
   ProgramID = glCreateProgram();
   GLuint vert = glCreateShader(GL_VERTEX_SHADER);
   GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);

   glShaderSource(vert, ARRAY_SIZE(vertexShaderSource), vertexShaderSource, 0);
   glShaderSource(frag, ARRAY_SIZE(fragmentShaderSource), fragmentShaderSource, 0);
   glCompileShader(vert);
   glCompileShader(frag);

   glAttachShader(ProgramID, vert);
   glAttachShader(ProgramID, frag);
   glLinkProgram(ProgramID);
   glDeleteShader(vert);
   glDeleteShader(frag);
   mvpMatrixLocation = glGetUniformLocation(ProgramID, "mvpMatrix");
   textureLocation = glGetUniformLocation(ProgramID, "texture");
   scaleLocation = glGetUniformLocation(ProgramID, "scale");
   brightnessLocation = glGetUniformLocation(ProgramID, "brightness");
   positionAttribLocation = glGetAttribLocation(ProgramID, "position");
   offsetAttribLocation = glGetAttribLocation(ProgramID, "offset");
   colourAttribLocation = glGetAttribLocation(ProgramID, "colour");
   packedTexCoordsAttribLocation = glGetAttribLocation(ProgramID, "packedTexCoords");
   MakeMVPMatrix(mvpMatrix, 0.0f, ALG_MAX_Y-1, ALG_MAX_X-1, 0.0f);
}

static void context_reset(void)
{
//fprintf(stderr, "Context reset!\n");
   rglgen_resolve_symbols(hw_render.get_proc_address);

   compile_program();
   CreateImage(DotWidth, DotHeight, DotImage, &DotTextureID);
   CreateImage(GlowWidth, GlowHeight, GlowImage, &GlowTextureID);
//   setup_vao();
#ifdef CORE
   context_alive = true;
#endif
}

static void context_destroy(void)
{
//   fprintf(stderr, "Context destroy!\n");

#ifdef CORE
   glDeleteVertexArrays(1, &vao);
   vao = 0;
   context_alive = false;
#endif

    if (vbo)
    {
        glDeleteBuffers(1, &vbo);
        vbo = 0;
    }
    if (DotTextureID)
    {
        glDeleteTextures(1, &DotTextureID);
        DotTextureID = 0;
    }
    if (GlowTextureID)
    {
        glDeleteTextures(1, &GlowTextureID);
        GlowTextureID = 0;
    }
    if (ProgramID)
    {
        glDeleteProgram(ProgramID);
        ProgramID = 0;
    }
   
}

#ifdef HAVE_OPENGLES
static bool retro_init_hw_context(bool useHardwareContext)
{
   if (useHardwareContext)
   {    
#if defined(HAVE_OPENGLES_3_1)
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES_VERSION;
   hw_render.version_major = 3;
   hw_render.version_minor = 1;
#elif defined(HAVE_OPENGLES3)
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES3;
#else
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES2;
#endif
   hw_render.context_reset = context_reset;
   hw_render.context_destroy = context_destroy;
   hw_render.depth = false;
   hw_render.stencil = false;
   hw_render.bottom_left_origin = true;
   }
   else
       hw_render.context_type = RETRO_HW_CONTEXT_NONE;
   
   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
      return false;

   return true;
}
#else
static bool retro_init_hw_context(bool useHardwareContext)
{
   if (useHardwareContext)
   {    
#if defined(CORE)
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
   hw_render.version_major = 3;
   hw_render.version_minor = 1;
#else
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
#endif
   hw_render.context_reset = context_reset;
   hw_render.context_destroy = context_destroy;
   hw_render.depth = false;
   hw_render.stencil = false;
   hw_render.bottom_left_origin = true;
   }
   else
       hw_render.context_type = RETRO_HW_CONTEXT_NONE;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
      return false;

   return true;
}
#endif

static bool set_rendering_context(bool useHardwareContext)
{
    if (useHardwareContext)
    {
        enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
        if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt) || !retro_init_hw_context(true))
        {
//            fprintf(stderr, "XRGB8888 is not supported or couldn't initialise HW context, using software renderer.\n");
            enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_0RGB1555;
            environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
            return false;
        }
        return true;
    }
    else
    {
        enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_0RGB1555;
        environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
        retro_init_hw_context(false);
        return true;
    }
}

static void check_variables(void)
{
   struct retro_variable var;
   struct retro_system_av_info av_info;

   var.value = NULL;
   var.key = "vecx_use_hw";
   
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
        if (!strcmp(var.value, "Hardware"))
        {
            if (!usingHWContext)
            {
                if (set_rendering_context(true))
                    usingHWContext = true;
            }
        }
        else
        {
            if (usingHWContext)
            {
                set_rendering_context(false);
                usingHWContext = false;
            }
        }
   }
   else
   {
       usingHWContext = false;
   }

   if (usingHWContext)
   {
        var.value = NULL;
        var.key   = "vecx_res_hw";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        {
            char *pch;
            char str[100];
            snprintf(str, sizeof(str), "%s", var.value);
            
            pch = strtok(str, "x");
            if (pch)
                WIDTH = strtoul(pch, NULL, 0);
            pch = strtok(NULL, "x");
            if (pch)
                HEIGHT = strtoul(pch, NULL, 0);

//            fprintf(stderr, "[libretro-test]: Got size: %u x %u.\n", WIDTH, HEIGHT);
            usingHWContext = true;
        }
        else
            usingHWContext = false;
   }
  
   if (usingHWContext)
   {
        var.value = NULL;
        var.key = "vecx_glow_brightness";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        {
            int value = atoi(var.value);
            if (value < 0) 
                value = 4;
            maxAlpha = value * 0.05f;
        }

        var.value = NULL;
        var.key = "vecx_line_brightness";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        {
            int value = atoi(var.value);
            if (value <= 0)
                value = 4;
            lineBrightness = value * 54.0f;
        }
        
        var.value = NULL;
        var.key = "vecx_line_width";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        {
            int value = atoi(var.value);
            if (value <= 0)
                value = 4;
            lineWidth = value * 18.75f;
        }
        
        var.value = NULL;
        var.key = "vecx_glow_width";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        {
            int value = atoi(var.value);
            if (value <= 0)
                value = 8;
            glowWidthMultiplier = value;
        }
   }
   else
   {
    var.value = NULL;
    var.key   = "vecx_res_multi";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "1"))
            {
                WIDTH = 330;
                HEIGHT = 410;
                point_size = 1;
            }
        else if (!strcmp(var.value, "2"))
            {
                WIDTH = 660;
                HEIGHT = 820;
                point_size = 2;
            }
        else if (!strcmp(var.value, "3"))
            {
                WIDTH = 990;
                HEIGHT = 1230;
                point_size = 2;
            }
        else if (!strcmp(var.value, "4"))
            {
                WIDTH = 1320;
                HEIGHT = 1640;
                point_size = 3;
            }
    }
   }
   retro_get_system_av_info(&av_info);
   environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
}

void retro_init(void)
{
   unsigned level = 5; 
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);

   check_variables();
}

size_t retro_serialize_size(void)
{
	return vecx_statesz();
}

bool retro_serialize(void *data, size_t size)
{
	return vecx_serialize((char*)data, size);
}

bool retro_unserialize(const void *data, size_t size)
{
	return vecx_deserialize((char*)data, size);
}

static unsigned char cart[65536];

bool retro_load_game(const struct retro_game_info *info)
{
   size_t cart_sz;
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "3" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "4" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "2" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "1" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "3" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "4" },

      { 0 }
   };

   if (!info)
      return false;

    if (usingHWContext)
    {
        usingHWContext = set_rendering_context(true);
    }
    else
        usingHWContext = !set_rendering_context(false);

// Hide options that don't apply to current renderer.    
    if (usingHWContext)
    {
        struct retro_core_option_display option_display;
        option_display.visible = false;
        option_display.key = "vecx_res_multi";
        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
    }
    else
    {
        struct retro_core_option_display option_display;
        option_display.visible = false;
        option_display.key = "vecx_res_hw";
        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
        option_display.key = "vecx_line_brightness";
        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
        option_display.key = "vecx_line_width";
        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
        option_display.key = "vecx_glow_brightness";
        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
        option_display.key = "vecx_glow_width";
        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
    }

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   e8910_init_sound();
   memset(framebuffer, 0, sizeof(framebuffer));

   /* start with a fresh BIOS copy */
	memcpy(rom, bios_data, bios_data_size);

   /* just memcpy buffer, ROMs are so tiny on Vectrex */
	cart_sz = sizeof(cart) / sizeof(cart[0]);

	if (info->data && info->size > 0 && info->size <= cart_sz)
   {
      int b;
      memset(cart, 0, cart_sz);
      memcpy(cart, info->data, info->size);
      for(b = 0; b < sizeof(cart); b++)
      {
	   set_cart(b, cart[b]);
      }

      vecx_reset();
      e8910_init_sound();

      return true;
   }

	return false;
}

void retro_unload_game(void)
{
    int b;
	memset(cart, 0, sizeof(cart) / sizeof(cart[0]));
	for(b = 0; b < sizeof(cart); b++)
	   set_cart(b, 0);
	vecx_reset();
}

void retro_reset(void)
{
	vecx_reset();
	e8910_init_sound();
}

static INLINE uint16_t RGB1555(int col)
{
    col >>= 2;  /* Lose the bottom two bits because we are squeezing 7 bits of colour into 5. */
    return col << 10 | col << 5 | col;
}

static INLINE void draw_point(int x, int y, unsigned char col)
{
   int psz = point_size;
   int sy, ey, sx, ex;

   if (psz == 1)
   {
      framebuffer[ (y * WIDTH) + x ] = RGB1555(col);
      return;
   }

   sy = y - psz > 0        ? y - psz : 0;
   ey = y + psz<= HEIGHT-1 ? y + psz : HEIGHT - 1;
   sx = x - psz > 0        ? x - psz : 0;
   ex = x + psz<= WIDTH -1 ? x + psz : WIDTH  - 1;

   for (y = sy; y <= ey; y++)
      for (x = sx; x <= ex; x++)
         if ( (x-sx) * (x-sx) + (y - sy) * (y - sy) <= psz * psz)
            framebuffer[ (y * WIDTH) + x ] = RGB1555(col);
}

static INLINE void draw_line(unsigned x0, unsigned y0, unsigned x1, unsigned y1, unsigned char col)
{
  int dx = abs((int)x1-(int)x0), sx = x0<x1 ? 1 : -1;
  int dy = abs((int)y1-(int)y0), sy = y0<y1 ? 1 : -1; 
  int err = (dx>dy ? dx : -dy)/2, e2;
 
  while(1)
   {
      draw_point(x0, y0, col);
    if (x0==x1 && y0==y1) break;
    e2 = err;
    if (e2 >-dx) { err -= dy; x0 += sx; }
    if (e2 < dy) { err += dx; y0 += sy; }
  }
}

static inline uint32_t MakeAll(float dx, float dy, int8_t col, uint8_t tc)
{
    return (((int8_t)(dx*64.0f+0.5f)) & 0xff) | (((int8_t)(dy*64.0f+0.5f) & 0xff) << 8) | ((col << 16)&0xff0000) | (tc << 24);
}

static inline float Dot2D(POINT a, POINT b)
{
    return a.x * b.x + a.y * b.y;
}

static inline POINT IntersectionPoint(POINT a, POINT b, POINT c, POINT d)
{
    POINT res;

    float a1 = b.y - a.y;
    float b1 = a.x - b.x;
    float c1 = a1 * a.x + b1 * a.y;

    float a2 = d.y - c.y;
    float b2 = c.x - d.x;
    float c2 = a2 * c.x + b2 * c.y;

    float determinant = a1 * b2 - a2 * b1;

    if (determinant == 0.0f)
    {
        res.x = 0.0f;
        res.y = 0.0f;
    }
    else
    {
        res.x = (b2 * c1 - b1 * c2) / determinant;
        res.y = (a1 * c2 - a2 * c1) / determinant;
    }

    return res;
}


void osint_render(void)
{
    if (!usingHWContext)
    {
        int i;
        unsigned char intensity;
        unsigned x0, x1, y0, y1;

        (void)intensity;

        memset(framebuffer, 0, BUFSZ * sizeof(unsigned short));

        /* rasterize list of vectors */
        for (i = 0; i < vector_draw_cnt; i++)
        {
            unsigned char intensity = vectors_draw[i].color;
            x0 = (float)vectors_draw[i].x0 / (float)ALG_MAX_X * (float)WIDTH;
            x1 = (float)vectors_draw[i].x1 / (float)ALG_MAX_X * (float)WIDTH;
            y0 = (float)vectors_draw[i].y0 / (float)ALG_MAX_Y * (float)HEIGHT;
            y1 = (float)vectors_draw[i].y1 / (float)ALG_MAX_Y * (float)HEIGHT;

            if (intensity == 128)
                continue;
            
            if (x0 - x1 == 0 && y0 - y1 == 0)
                draw_point(x0, y0, intensity);
            else
                draw_line(x0, y0, x1, y1, intensity);
        }
    }
    else
    {
        GLint scissorTestEnabled = glIsEnabled(GL_SCISSOR_TEST);
        GLint scissorBox[4];
        glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
//        fprintf(stderr, "\rsbwidth=%d, sbheight=%d", scissorBox[2], scissorBox[3]);
        glBindFramebuffer(RARCH_GL_FRAMEBUFFER, hw_render.get_current_framebuffer());

/* The texture backing the framebuffer is square and a power-of-two, we only draw in the bottom left of it.
   We use the scissor box so the glClearColor() only updates the part of the texture that we use rather than all the texture.
   This saves memory bandwidth, which is very important on low memory bandwidth and/or tile based GPUs.
*/
        glScissor(0, 0, WIDTH, HEIGHT);
        glEnable(GL_SCISSOR_TEST);
        glViewport(0, 0, WIDTH, HEIGHT);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f-maxAlpha);
        
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        
        glUseProgram(ProgramID);
        glUniformMatrix4fv(mvpMatrixLocation, 1, GL_FALSE, mvpMatrix);
        glVertexAttribPointer(positionAttribLocation, 2, GL_UNSIGNED_SHORT, GL_FALSE, sizeof(GLVERTEX), &(verticies[0].pos));
        glEnableVertexAttribArray(positionAttribLocation);
        glVertexAttribPointer(offsetAttribLocation, 2, GL_BYTE, GL_FALSE, sizeof(GLVERTEX), &(verticies[0].offsets));
        glEnableVertexAttribArray(offsetAttribLocation);
        glVertexAttribPointer(colourAttribLocation, 1, GL_BYTE, GL_FALSE, sizeof(GLVERTEX), &(verticies[0].colour));
        glEnableVertexAttribArray(colourAttribLocation);
        glVertexAttribPointer(packedTexCoordsAttribLocation, 1, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GLVERTEX), &(verticies[0].packedTexCoords));
        glEnableVertexAttribArray(packedTexCoordsAttribLocation);
        
        GLint numVerts = 0;
        int continuing = 0;
        
        int colour = 0;

        float dx = 0.0f;
        float dy = 0.0f;
        
        for (int i=0; i<vector_draw_cnt; i++)
        {
            colour = vectors_draw[i].color;
            if (colour == 0 || colour > 127)
                continue;

            // Is this vector a point?
            if (vectors_draw[i].x0 == vectors_draw[i].x1 && vectors_draw[i].y0 == vectors_draw[i].y1
            // That isn't joining two lines.
            && (vectors_draw[i].x0 != vectors_draw[i-1].x1 || vectors_draw[i].x1 != vectors_draw[i+1].x0 ||
                vectors_draw[i].y0 != vectors_draw[i-1].y1 || vectors_draw[i].y1 != vectors_draw[i+1].y0))
    //        if (vectors_draw[i].p0 == vectors_draw[i].p1
    //            && (vectors_draw[i].p0 != vectors_draw[i-1].p1 || vectors_draw[i].p1 != vectors_draw[i+1].p0))
            {
                verticies[numVerts].pos = vectors_draw[i].x0 | vectors_draw[i].y0 << 16;
                verticies[numVerts].rest = MakeAll(-dotScale, dotScale, colour, 0x02);
                numVerts++;
                verticies[numVerts].pos = vectors_draw[i].x0 | vectors_draw[i].y0 << 16;
                verticies[numVerts].rest = MakeAll(dotScale, dotScale, colour, 0x22);
                numVerts++;
                verticies[numVerts].pos = vectors_draw[i].x0 | vectors_draw[i].y0 << 16;
                verticies[numVerts].rest = MakeAll(-dotScale, -dotScale, colour, 0x00);
                numVerts++;
                verticies[numVerts] = verticies[numVerts-2];
                numVerts++;
                verticies[numVerts] = verticies[numVerts-2];
                numVerts++;
                verticies[numVerts].pos = vectors_draw[i].x0 | vectors_draw[i].y0 << 16;
                verticies[numVerts].rest = MakeAll(dotScale, -dotScale, colour, 0x20);
                numVerts++;

                continuing = 0;

                continue;               // Loop round to the next vector.
            }
            
            // Draw end cap if we are not continuing the line
            if (!continuing)
            {
                dx = vectors_draw[i].x1 - vectors_draw[i].x0;
                dy = vectors_draw[i].y1 - vectors_draw[i].y0; 
                float length = sqrt(dx*dx+dy*dy);
                dx /= length;
                dy /= length;
                
                verticies[numVerts].pos = vectors_draw[i].x0 | vectors_draw[i].y0 << 16;
                verticies[numVerts].rest = MakeAll((-dy-dx), (dx-dy), colour, 0x20);
                numVerts++;
                verticies[numVerts].pos = vectors_draw[i].x0 | vectors_draw[i].y0 << 16;
                verticies[numVerts].rest = MakeAll((dy-dx), (-dx-dy), colour, 0x22);
                numVerts++;
                verticies[numVerts].pos = vectors_draw[i].x0 | vectors_draw[i].y0 << 16;
                verticies[numVerts].rest = MakeAll(-dy, dx, colour, 0x10);
                numVerts++;
                verticies[numVerts] = verticies[numVerts-2];
                numVerts++;
                verticies[numVerts] = verticies[numVerts-2];
                numVerts++;
                verticies[numVerts].pos = vectors_draw[i].x0 | vectors_draw[i].y0 << 16;
                verticies[numVerts].rest = MakeAll(dy, -dx, colour, 0x12);
                numVerts++;
            }
            
            float nextDx = dx;
            float nextDy = dy;
        
            // Are we contiguous with the next vector?
            if (i < vector_draw_cnt-1 &&                                                                        // We are not the last vector...
    //            (vectors_draw[i].p1 == vectors_draw[i+1].p0) &&
    //            (vectors_draw[i+1].p0 != vectors_draw[i+1].p1))
                (vectors_draw[i].x1 == vectors_draw[i+1].x0 && vectors_draw[i].y1 == vectors_draw[i+1].y0) &&   // ...are connected to next vector...
                (vectors_draw[i+1].x0 != vectors_draw[i+1].x1 || vectors_draw[i+1].y0 != vectors_draw[i+1].y1)) // ...and the next vector isn't a point.
            {
                float localNextDx = vectors_draw[i+1].x1 - vectors_draw[i+1].x0;
                float localNextDy = vectors_draw[i+1].y1 - vectors_draw[i+1].y0; 
                float length = sqrt(localNextDx*localNextDx+localNextDy*localNextDy);
                localNextDx /= length;
                localNextDy /= length;
                
                POINT thisVec = {dx, dy};
                POINT nextVec = {localNextDx, localNextDy};
                
                float dot = Dot2D(thisVec, nextVec);

                if (dot > 0.99f)   // If (nearly) parallel.
                {
                    vectors_draw[i].x1 = (vectors_draw[i].x1 + vectors_draw[i+1].x0) / 2;
                    vectors_draw[i].y1 = (vectors_draw[i].y1 + vectors_draw[i+1].y0) / 2;
                    nextDx = (dx + localNextDx) / 2.0f;
                    nextDy = (dy + localNextDy) / 2.0f;
                    
                    continuing = 1;
                    dx = localNextDx;
                    dy = localNextDy;
                }
                else if (dot >= 0.0f)   // If change in angle is less than or equal to 90 degrees.
                {
                    POINT a = {vectors_draw[i].x0-dy, vectors_draw[i].y0+dx};
                    POINT b = {vectors_draw[i].x1-dy, vectors_draw[i].y1+dx};
                    POINT c = {vectors_draw[i+1].x0-localNextDy, vectors_draw[i+1].y0+localNextDx};
                    POINT d = {vectors_draw[i+1].x1-localNextDy, vectors_draw[i+1].y1+localNextDx};

                    POINT p0 = IntersectionPoint(a, b, c, d);
                    
                    POINT a1 = {vectors_draw[i].x0+dy, vectors_draw[i].y0-dx};
                    POINT b1 = {vectors_draw[i].x1+dy, vectors_draw[i].y1-dx};
                    POINT c1 = {vectors_draw[i+1].x0+localNextDy, vectors_draw[i+1].y0-localNextDx};
                    POINT d1 = {vectors_draw[i+1].x1+localNextDy, vectors_draw[i+1].y1-localNextDx};
                    
                    POINT p1 = IntersectionPoint(a1, b1, c1, d1);
                    
                    vectors_draw[i].x1 = (p0.x + p1.x) / 2.0f;
                    vectors_draw[i+1].x0 = vectors_draw[i].x1;
                    vectors_draw[i].y1 = (p0.y + p1.y) / 2.0f;
                    vectors_draw[i+1].y0 = vectors_draw[i].y1;
                    nextDy = ((p1.x - p0.x) / 2.0f);
                    nextDx = -((p1.y - p0.y) / 2.0f);

                    continuing = 1;
                    dx = localNextDx;
                    dy = localNextDy;
                }
                else    // Angle between lines is too great - treat them as seperate lines.
                {
                    continuing = 0;
                }
                
            }
            else
                continuing = 0;
            
            // The previous two verticies are the first two we need for the next line.
            // This applies whether they were part of the last line or the end cap of this line.
            verticies[numVerts] = verticies[numVerts-2];
            verticies[numVerts].colour = colour;
            numVerts++;
            verticies[numVerts] = verticies[numVerts-2];
            verticies[numVerts].colour = colour;
            numVerts++;
            
            verticies[numVerts].pos = vectors_draw[i].x1 | vectors_draw[i].y1 << 16;
            verticies[numVerts].rest = MakeAll(-nextDy, nextDx, colour, 0x10);
            numVerts++;
            verticies[numVerts] = verticies[numVerts-2];
            verticies[numVerts].colour = colour;
            numVerts++;
            verticies[numVerts] = verticies[numVerts-2];
            verticies[numVerts].colour = colour;
            numVerts++;
            verticies[numVerts].pos = vectors_draw[i].x1 | vectors_draw[i].y1 << 16;
            verticies[numVerts].rest = MakeAll(nextDy, -nextDx, colour, 0x12);
            numVerts++;

            if (!continuing)
            {
                // And now the end cap.
                verticies[numVerts] = verticies[numVerts-2];
                verticies[numVerts].colour = colour;
                numVerts++;
                verticies[numVerts] = verticies[numVerts-2];
                verticies[numVerts].colour = colour;
                numVerts++;
                verticies[numVerts].pos = vectors_draw[i].x1 | vectors_draw[i].y1 << 16;
                verticies[numVerts].rest = MakeAll((-nextDy+nextDx), (nextDx+nextDy), colour, 0x00);
                numVerts++;
                verticies[numVerts] = verticies[numVerts-2];
                numVerts++;
                verticies[numVerts] = verticies[numVerts-2];
                numVerts++;
                verticies[numVerts].pos = vectors_draw[i].x1 | vectors_draw[i].y1 << 16;
                verticies[numVerts].rest = MakeAll((nextDy+nextDx), (-nextDx+nextDy), colour, 0x02);
                numVerts++;
            }
        }

        // Draw the glowing lines if enabled.
        if (maxAlpha > 0.0f)
        {
            glEnable(GL_TEXTURE_2D);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, GlowTextureID);
            glUniform1i(textureLocation, 0);
            glUniform1f(scaleLocation, lineWidth * glowWidthMultiplier);
            glUniform1f(brightnessLocation, glowBrightness);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE);
            glDrawArrays(GL_TRIANGLES, 0, numVerts);
        }

        // Draw the lines.
        glEnable(GL_TEXTURE_2D);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, DotTextureID);
        glUniform1i(textureLocation, 0);
        glUniform1f(scaleLocation, lineWidth);
        glUniform1f(brightnessLocation, lineBrightness);
        glBlendFunc(GL_ONE, GL_ONE);
        glDrawArrays(GL_TRIANGLES, 0, numVerts);

        glDisableVertexAttribArray(positionAttribLocation);
        glDisableVertexAttribArray(colourAttribLocation);
        glDisableVertexAttribArray(offsetAttribLocation);
        glDisableVertexAttribArray(packedTexCoordsAttribLocation);
        
        glUseProgram(0);
        
        /* Restore the old scissor box state. */
        if (scissorTestEnabled == GL_FALSE)
            glDisable(GL_SCISSOR_TEST);
        glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
        glDisable(GL_BLEND);
        
    	/* Start rendering ASAP by hinting to GL start get rendering now. */
        glFlush();
    }
}

/* NOTE: issue with this core atm. (and thus, emulation) is partly input
 * (lightpens, analog axes etc. plugged into the different ports)
 * and statemanagement (as in, there is none currently) */

void retro_run(void)
{
	int i;
	bool updated = false;
	uint8_t buffer[882];

	/* poll input and update states;
	   buttons (snd_regs[14], 4 buttons/pl => 4 bits starting from LSB, |= for rel. &= ~ for push)
	   analog stick (alg_jch0, alg_jch1, => -1 (0x00) .. 0 (0x80) .. 1 (0xff)) */
	poll_cb();

	/* Player 1 */

	if      (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT ))
		alg_jch0 = 0x00;
	else if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
		alg_jch0 = 0xff;
	else
		alg_jch0 = 0x80;

	if      (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP   ))
		alg_jch1 = 0xff;
	else if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN ))
		alg_jch1 = 0x00;
	else
		alg_jch1 = 0x80;


	if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A ))
		snd_regs[14] &= ~1;
	else
		snd_regs[14] |= 1;

	if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B ))
		snd_regs[14] &= ~2;
	else
		snd_regs[14] |= 2;

	if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X ))
		snd_regs[14] &= ~4;
	else
		snd_regs[14] |= 4;

	if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y ))
		snd_regs[14] &= ~8;
	else
		snd_regs[14] |= 8;

	/* Player 2 */
	if      (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT ))
		alg_jch2 = 0x00;
	else if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
		alg_jch2 = 0xff;
	else
		alg_jch2 = 0x80;

	if      (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP   ))
		alg_jch3 = 0xff;
	else if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN ))
		alg_jch3 = 0x00;
	else
		alg_jch3 = 0x80;

	if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A ))
		snd_regs[14] &= ~16;
	else
		snd_regs[14] |= 16;

	if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B ))
		snd_regs[14] &= ~32;
	else
		snd_regs[14] |= 32;

	if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X ))
		snd_regs[14] &= ~64;
	else
		snd_regs[14] |= 64;

	if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y ))
		snd_regs[14] &= ~128;
	else
		snd_regs[14] |= 128;

	int ret = vecx_emu(30000); /* 1500000 / 1000 * 20 */

	e8910_callback(NULL, buffer, 882);

	for (i = 0; i < 882; i++)
	{
		short convs = (buffer[i] << 8) - 0x7ff;
		audio_cb(convs, convs);
	}

    if (usingHWContext)
        video_cb(ret ? RETRO_HW_FRAME_BUFFER_VALID : NULL, WIDTH, HEIGHT, 0);
    else
        video_cb(framebuffer, WIDTH, HEIGHT, WIDTH * sizeof(unsigned short));
    
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
	{
		check_variables();
	}
}
