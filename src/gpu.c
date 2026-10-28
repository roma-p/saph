// Shader runner shared by all platforms. See gpu.h.

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <GL/glcorearb.h>

#include "gpu.h"

// ---------------------------------------------------------------- GL functions

#define GL_FUNCTIONS(X)                                                          \
    X(PFNGLGETSTRINGPROC, GetString)                                             \
    X(PFNGLGETERRORPROC, GetError)                                               \
    X(PFNGLGETINTEGERVPROC, GetIntegerv)                                         \
    X(PFNGLVIEWPORTPROC, Viewport)                                               \
    X(PFNGLPIXELSTOREIPROC, PixelStorei)                                         \
    X(PFNGLGENTEXTURESPROC, GenTextures)                                         \
    X(PFNGLDELETETEXTURESPROC, DeleteTextures)                                   \
    X(PFNGLBINDTEXTUREPROC, BindTexture)                                         \
    X(PFNGLACTIVETEXTUREPROC, ActiveTexture)                                     \
    X(PFNGLTEXIMAGE2DPROC, TexImage2D)                                           \
    X(PFNGLTEXPARAMETERIPROC, TexParameteri)                                     \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers)                                 \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer)                                 \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D)                       \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus)                   \
    X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays)                                 \
    X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray)                                 \
    X(PFNGLDRAWARRAYSPROC, DrawArrays)                                           \
    X(PFNGLREADPIXELSPROC, ReadPixels)                                           \
    X(PFNGLCREATESHADERPROC, CreateShader)                                       \
    X(PFNGLSHADERSOURCEPROC, ShaderSource)                                       \
    X(PFNGLCOMPILESHADERPROC, CompileShader)                                     \
    X(PFNGLGETSHADERIVPROC, GetShaderiv)                                         \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog)                               \
    X(PFNGLDELETESHADERPROC, DeleteShader)                                       \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram)                                     \
    X(PFNGLATTACHSHADERPROC, AttachShader)                                       \
    X(PFNGLLINKPROGRAMPROC, LinkProgram)                                         \
    X(PFNGLDELETEPROGRAMPROC, DeleteProgram)                                     \
    X(PFNGLDELETEVERTEXARRAYSPROC, DeleteVertexArrays)                           \
    X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers)                           \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv)                                       \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog)                             \
    X(PFNGLUSEPROGRAMPROC, UseProgram)                                           \
    X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation)                           \
    X(PFNGLUNIFORM1IPROC, Uniform1i)                                             \
    X(PFNGLUNIFORM1FPROC, Uniform1f)

#define DECLARE(type, name) static type gl##name;
GL_FUNCTIONS(DECLARE)
#undef DECLARE

static int load_functions(char* err, size_t err_size) {
#define LOAD(type, name)                                                     \
    gl##name = (type)ms_gl_proc("gl" #name);                                 \
    if (!gl##name) {                                                         \
        snprintf(err, err_size, "OpenGL function gl" #name " is missing");   \
        return 0;                                                            \
    }
    GL_FUNCTIONS(LOAD)
#undef LOAD
    return 1;
}

// ---------------------------------------------------------------- GPU thread
// One thread owns the OpenGL context; callers hand it a task and wait.

typedef struct {
    void (*run)(void* arg);
    void* arg;
    int done;
} Task;

static void start(void);  // sets up the context, first thing on the GPU thread

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static int thread_running;
static int quit;  // set by the last task before the thread exits
static Task* pending;

static void* gpu_thread(void* unused) {
    (void)unused;
    start();
    pthread_mutex_lock(&lock);
    while (!quit) {
        while (!pending || pending->done) pthread_cond_wait(&changed, &lock);
        Task* task = pending;
        pthread_mutex_unlock(&lock);
        task->run(task->arg);
        pthread_mutex_lock(&lock);
        task->done = 1;
        pthread_cond_broadcast(&changed);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

// Runs `run(arg)` on the GPU thread (starting it if needed) and waits for it.
// Returns 0 if the thread cannot be started.
static int run_on_gpu_thread(void (*run)(void*), void* arg) {
    Task task = {run, arg, 0};
    pthread_mutex_lock(&lock);
    if (!thread_running) {
        quit = 0;
        if (pthread_create(&thread, NULL, gpu_thread, NULL) != 0) {
            pthread_mutex_unlock(&lock);
            return 0;
        }
        thread_running = 1;
    }
    while (pending && !pending->done) pthread_cond_wait(&changed, &lock);  // one task at a time
    pending = &task;
    pthread_cond_broadcast(&changed);
    while (!task.done) pthread_cond_wait(&changed, &lock);
    pending = NULL;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return 1;
}

// ---------------------------------------------------------------- state (GPU thread only)

static int ready;
static char info[2048] = "GPU not started yet";
static GLuint vertex_array, framebuffer, vertex_shader;
static GLint max_size, max_units;

// Compiled programs, by shader source address (shaders are static strings).
// 256 covers every pass of every effect.
static struct { const char* source; GLuint program; } programs[256];
static int program_count;

// Draws one triangle covering the whole viewport.
static const char* vertex_source =
    "#version 410 core\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char* source, char* err, size_t err_size) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        snprintf(err, err_size, "shader does not compile: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint get_program(const char* source, char* err, size_t err_size) {
    for (int i = 0; i < program_count; ++i)
        if (programs[i].source == source) return programs[i].program;
    if (program_count == (int)(sizeof programs / sizeof programs[0])) {
        snprintf(err, err_size, "too many shaders");
        return 0;
    }
    GLuint fragment = compile(GL_FRAGMENT_SHADER, source, err, err_size);
    if (!fragment) return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(fragment);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        snprintf(err, err_size, "shader does not link: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    programs[program_count].source = source;
    programs[program_count].program = program;
    ++program_count;
    return program;
}

static void start(void) {
    char err[2048];
    if (!ms_gl_create_context(err, sizeof err) || !load_functions(err, sizeof err)) {
        snprintf(info, sizeof info, "OpenGL setup failed: %s", err);
        return;
    }
    const char* version = (const char*)glGetString(GL_VERSION);
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    int major = 0, minor = 0;
    if (!version || sscanf(version, "%d.%d", &major, &minor) != 2 || major * 10 + minor < 41) {
        snprintf(info, sizeof info, "OpenGL 4.1 needed, got %s", version ? version : "nothing");
        return;
    }
    vertex_shader = compile(GL_VERTEX_SHADER, vertex_source, err, sizeof err);
    if (!vertex_shader) {
        snprintf(info, sizeof info, "%s", err);
        return;
    }
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &max_units);
    glGenVertexArrays(1, &vertex_array);
    glGenFramebuffers(1, &framebuffer);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    snprintf(info, sizeof info, "OpenGL %s | %s | %s", version, renderer ? renderer : "?", ms_gl_api);
    fprintf(stderr, "[MillSaph] %s\n", info);
    ready = 1;
}

static void nothing(void* unused) { (void)unused; }

const char* ms_gpu_info(void) {
    if (!run_on_gpu_thread(nothing, NULL))  // starts the GPU thread on first use
        return "cannot start the GPU thread";
    return info;
}

// Last task of the GPU thread: release everything, then let the thread exit.
static void stop(void* unused) {
    (void)unused;
    if (ready) {
        for (int i = 0; i < program_count; ++i) glDeleteProgram(programs[i].program);
        glDeleteShader(vertex_shader);
        glDeleteVertexArrays(1, &vertex_array);
        glDeleteFramebuffers(1, &framebuffer);
    }
    ms_gl_destroy_context();
    program_count = 0;
    ready = 0;
    snprintf(info, sizeof info, "GPU not started yet");
    quit = 1;  // read by the thread under the lock, right after this task
}

void ms_gpu_shutdown(void) {
    pthread_mutex_lock(&lock);
    int running = thread_running;
    pthread_mutex_unlock(&lock);
    if (!running) return;
    run_on_gpu_thread(stop, NULL);
    pthread_join(thread, NULL);
    pthread_mutex_lock(&lock);
    thread_running = 0;
    pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------- passes

typedef struct {
    const char* const* shaders;
    int pass_count;
    const float* input;
    int width, height;
    const MsUniform* uniforms;
    int uniform_count;
    float* output;
    char* err;
    size_t err_size;
    int ok;
} Job;

#define MAX_TEXTURES 32  // the input + pass results in one job

// Linear filtering (for blurs; texelFetch is not affected), edges clamped.
static GLuint make_texture(int width, int height, const float* pixels) {
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, pixels);
    return texture;
}

// Draws one pass into `target`: binds every texture so far (inputs, then earlier
// pass results) to its own unit and points the matching sampler at it.
static int draw_pass(Job* job, int pass, GLuint program, const GLuint* textures, const char* const* names,
                     int texture_count, GLuint target) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        snprintf(job->err, job->err_size, "pass %d: cannot render into a float image", pass + 1);
        return 0;
    }
    glUseProgram(program);
    for (int i = 0; i < texture_count; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glUniform1i(glGetUniformLocation(program, names[i]), i);  // -1 (unused) is ignored
    }
    for (int i = 0; i < job->uniform_count; ++i)
        glUniform1f(glGetUniformLocation(program, job->uniforms[i].name), job->uniforms[i].value);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return 1;
}

static void run_job(void* arg) {
    Job* job = arg;
    if (!ready) {
        snprintf(job->err, job->err_size, "%s", info);
        return;
    }
    if (job->width > max_size || job->height > max_size) {
        snprintf(job->err, job->err_size, "image larger than the GPU limit (%d pixels)", max_size);
        return;
    }
    const int input_count = 1;                                // source
    const int texture_count = input_count + job->pass_count;  // the input, then one result per pass
    if (job->pass_count < 1 || texture_count > MAX_TEXTURES || texture_count - 1 > max_units) {
        snprintf(job->err, job->err_size, "%d passes: too many for the GPU", job->pass_count);
        return;
    }
    GLuint programs_used[MAX_TEXTURES];
    for (int p = 0; p < job->pass_count; ++p) {
        programs_used[p] = get_program(job->shaders[p], job->err, job->err_size);
        if (!programs_used[p]) return;
    }

    // textures[i] is read in the shaders as names[i].
    glActiveTexture(GL_TEXTURE0);
    GLuint textures[MAX_TEXTURES];
    const char* names[MAX_TEXTURES];
    char pass_names[MAX_TEXTURES][16];
    textures[0] = make_texture(job->width, job->height, job->input);
    names[0] = "source";
    for (int p = 0; p < job->pass_count; ++p) {
        const int i = input_count + p;
        textures[i] = make_texture(job->width, job->height, NULL);
        snprintf(pass_names[p], sizeof pass_names[p], "pass%d", p + 1);
        names[i] = pass_names[p];
    }

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, job->width, job->height);
    glBindVertexArray(vertex_array);
    int ok = 1;
    for (int p = 0; p < job->pass_count && ok; ++p) {
        // Pass p reads the inputs and the results of passes 1..p, and writes its own.
        const int target = input_count + p;
        ok = draw_pass(job, p, programs_used[p], textures, names, target, textures[target]);
    }
    if (ok) {
        glReadPixels(0, 0, job->width, job->height, GL_RGBA, GL_FLOAT, job->output);
        GLenum e = glGetError();
        if (e != GL_NO_ERROR) snprintf(job->err, job->err_size, "OpenGL error 0x%04X", e);
        else job->ok = 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteTextures(texture_count, textures);
}

int ms_gpu_run(const char* const* shaders, int pass_count, const float* input,
               int width, int height, const MsUniform* uniforms, int uniform_count,
               float* output, char* err, size_t err_size) {
    Job job = {shaders, pass_count, input, width, height, uniforms, uniform_count, output, err, err_size, 0};
    if (!run_on_gpu_thread(run_job, &job)) {
        snprintf(err, err_size, "cannot start the GPU thread");
        return 0;
    }
    return job.ok;
}
