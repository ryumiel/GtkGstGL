#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <gst/gst.h>
#include <gst/gl/gl.h>

#include <GL/gl.h>
#include <GL/glx.h>

#if GST_GL_HAVE_WINDOW_X11 && defined (GDK_WINDOWING_X11)
#include <X11/Xlib.h>
#include <gst/gl/x11/gstgldisplay_x11.h>
#endif

#define GLAREA_ERROR (glarea_error_quark ())

#define GLCOMMAND(command) command; _print_OpenGL_error(__FILE__, __LINE__)

#define print_OpenGL_Error() _print_OpenGL_error(__FILE__, __LINE__)

static int _print_OpenGL_error(char *file, int line)
{
  GLenum glErr;
  int    retCode = 0;

  glErr = glGetError();
  if (glErr != GL_NO_ERROR)
  {
    printf("glError in file %s @ line %d: %d\n",
	     file, line, glErr);
    retCode = 1;
  }
  return retCode;
}

typedef enum {
  GLAREA_ERROR_SHADER_COMPILATION,
  GLAREA_ERROR_SHADER_LINK
} GlareaError;

G_DEFINE_QUARK (glarea-error, glarea_error)

static char const *vertex_shader_str =
      "attribute vec3 aVertexPosition;   \n"
      "attribute vec2 aTextureCoord;   \n"
      "varying vec2 vTexureCoord;     \n"
      "void main()                  \n"
      "{                            \n"
      "   gl_Position = vec4(aVertexPosition, 1.0); \n"
      "   vTexureCoord = aTextureCoord;  \n"
      "}                            \n";

static char const *fragment_shader_str =
      "#ifdef GL_ES                                        \n"
      "precision mediump float;                            \n"
      "#endif                                              \n"
      "varying vec2 vTexureCoord;                            \n"
      "uniform sampler2D tex;                              \n"
      "void main()                                         \n"
      "{                                                   \n"
      "  gl_FragColor = texture2D( tex, vTexureCoord );      \n"
      "}                                                   \n";

struct vertex_info {
  float position[3];
  float texture_coord[2];
};

static const struct vertex_info vertex_data[] = {
  { { -1.0f,  1.0f,  0.0f }, { 0.0f, 0.0f } },
  { { -1.0f, -1.0f,  0.0f }, { 0.0f, 1.0f } },
  { {  1.0f, -1.0f,  0.0f }, { 1.0f, 1.0f } },
  { {  1.0f,  1.0f,  0.0f }, { 1.0f, 0.0f } },
};

static GLushort const vertex_indice[] = {
 0, 1, 2, 0, 2, 3,
};

#define GST_WEBKIT_VIDEO_FORMATS "{ RGBA }" \

#define GST_WEBKIT_VIDEO_CAPS \
    "video/x-raw(" GST_CAPS_FEATURE_MEMORY_GL_MEMORY "), "              \
    "format = (string) " GST_WEBKIT_VIDEO_FORMATS ", "                  \
    "width = " GST_VIDEO_SIZE_RANGE ", "                                \
    "height = " GST_VIDEO_SIZE_RANGE ", "                               \
    "framerate = " GST_VIDEO_FPS_RANGE

typedef struct {
  GstVideoFrame video_frame;
  guint texture;
  GstGLWindow* gst_window;
} TextureBuffer;

static struct {
  gchar *uri;

  GMutex draw_mutex;
  GstSample* sample;
  GstElement* pipeline;

  TextureBuffer* current_buffer;
  TextureBuffer* pending_buffer;

  GAsyncQueue *queue_input_buf;
  GAsyncQueue *queue_output_buf;

  GLXContext gl_context;
  Display *display;
  GstGLContext *gst_gl_context;
  GstGLDisplay *gst_gl_display;

  GtkWidget *gl_area;
  guint vertex_buffer;
  guint indice_buffer;
  guint program;
  guint vertex_pos_attrib;
  guint texture_coord_attrib;
  guint texture_attrib;
} scene_info;

static void
init_buffers (guint  vertex_pos_attrib,
              guint  texture_coord_attrib,
              guint *indice_buffer_out,
              guint *vertex_buffer_out)
{
  guint vertex_buffer, indice_buffer;

  GLCOMMAND(glGenBuffers (1, &vertex_buffer));
  GLCOMMAND(glBindBuffer (GL_ARRAY_BUFFER, vertex_buffer));
  GLCOMMAND(glBufferData (GL_ARRAY_BUFFER, sizeof (vertex_data), vertex_data, GL_STATIC_DRAW));

  /* enable and set the position attribute */
  GLCOMMAND(glEnableVertexAttribArray (vertex_pos_attrib));
  GLCOMMAND(glVertexAttribPointer (vertex_pos_attrib, 3, GL_FLOAT, GL_FALSE, sizeof (struct vertex_info), (GLvoid *) (G_STRUCT_OFFSET (struct vertex_info, position))));

  /* enable and set the color attribute */
  GLCOMMAND(glEnableVertexAttribArray (texture_coord_attrib));
  GLCOMMAND(glVertexAttribPointer (texture_coord_attrib, 2, GL_FLOAT, GL_FALSE, sizeof (struct vertex_info), (GLvoid *) (G_STRUCT_OFFSET (struct vertex_info, texture_coord))));

  GLCOMMAND(glGenBuffers (1, &indice_buffer));
  GLCOMMAND(glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, indice_buffer));
  GLCOMMAND(glBufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof (vertex_indice), vertex_indice, GL_STATIC_DRAW));

  /* reset the state; we will re-enable buffers when needed */
  GLCOMMAND(glBindBuffer (GL_ARRAY_BUFFER, 0));
  GLCOMMAND(glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0));

  if (vertex_buffer_out != NULL)
    *vertex_buffer_out = vertex_buffer;
  if (indice_buffer_out != NULL)
    *indice_buffer_out = indice_buffer;
}

static guint
create_shader (int          shader_type,
               const char  *source,
               GError     **error,
               guint       *shader_out)
{
  guint shader = glCreateShader (shader_type);
  GLCOMMAND(glShaderSource (shader, 1, &source, NULL));
  GLCOMMAND(glCompileShader (shader));

  int status;
  glGetShaderiv (shader, GL_COMPILE_STATUS, &status);
  if (status == GL_FALSE)
    {
      int log_len;
      glGetShaderiv (shader, GL_INFO_LOG_LENGTH, &log_len);

      char *buffer = g_malloc (log_len + 1);
      glGetShaderInfoLog (shader, log_len, NULL, buffer);

      g_set_error (error, GLAREA_ERROR, GLAREA_ERROR_SHADER_COMPILATION,
                   "Compilation failure in %s shader: %s",
                   shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment",
                   buffer);

      g_free (buffer);

      glDeleteShader (shader);
      shader = 0;
    }

  if (shader_out != NULL)
    *shader_out = shader;

  return shader != 0;
}

static gboolean
init_shaders (guint   *program_out,
              guint   *vertex_pos_attrib_out,
              guint   *texture_coord_attrib_out,
              guint   *texture_attrib_out,
              GError **error)
{
  guint program = 0;
  guint vertex_pos_attrib = 0;
  guint texture_coord_attrib = 0;
  guint texture_attrib = 0;
  guint vertex = 0, fragment = 0;

  /* load the vertex shader */
  create_shader (GL_VERTEX_SHADER, vertex_shader_str, error, &vertex);
  if (vertex == 0)
    goto out;

  /* load the fragment shader */
  create_shader (GL_FRAGMENT_SHADER, fragment_shader_str, error, &fragment);
  if (fragment == 0)
    goto out;

  /* link the vertex and fragment shaders together */
  program = glCreateProgram ();
  GLCOMMAND(glAttachShader (program, vertex));
  GLCOMMAND(glAttachShader (program, fragment));
  GLCOMMAND(glLinkProgram (program));

  int status = 0;
  GLCOMMAND(glGetProgramiv (program, GL_LINK_STATUS, &status));
  if (status == GL_FALSE)
    {
      int log_len = 0;
      glGetProgramiv (program, GL_INFO_LOG_LENGTH, &log_len);

      char *buffer = g_malloc (log_len + 1);
      glGetProgramInfoLog (program, log_len, NULL, buffer);

      g_set_error (error, GLAREA_ERROR, GLAREA_ERROR_SHADER_LINK,
                   "Linking failure in program: %s", buffer);

      g_free (buffer);

      glDeleteProgram (program);
      program = 0;

      goto out;
    }

  vertex_pos_attrib = glGetAttribLocation (program, "aVertexPosition");
  texture_coord_attrib = glGetAttribLocation (program, "aTextureCoord");
  texture_attrib = glGetUniformLocation (program, "tex");

  /* the individual shaders can be detached and destroyed */
  GLCOMMAND(glDetachShader (program, vertex));
  GLCOMMAND(glDetachShader (program, fragment));

out:
  if (vertex != 0)
    glDeleteShader (vertex);
  if (fragment != 0)
    glDeleteShader (fragment);

  if (program_out != NULL)
    *program_out = program;
  if (vertex_pos_attrib_out != NULL)
    *vertex_pos_attrib_out = vertex_pos_attrib;
  if (texture_coord_attrib_out != NULL)
    *texture_coord_attrib_out = texture_coord_attrib;
  if (texture_attrib_out != NULL)
    *texture_attrib_out = texture_attrib;

  return program != 0;
}

static void
realize (GtkWidget *widget)
{
  /* we need to ensure that the GdkGLContext is set before calling GL API */
  gtk_gl_area_make_current (GTK_GL_AREA (widget));

  /* if the GtkGLArea is in an error state we don't do anything */
  if (gtk_gl_area_get_error (GTK_GL_AREA (widget)) != NULL)
    return;

  /* initialize the shaders and retrieve the program data */
  GError *error = NULL;
  if (!init_shaders (&scene_info.program,
                     &scene_info.vertex_pos_attrib,
                     &scene_info.texture_coord_attrib,
                     &scene_info.texture_attrib,
                     &error))
    {
      /* set the GtkGLArea in error state, so we'll see the error message
       * rendered inside the viewport
       */
      gtk_gl_area_set_error (GTK_GL_AREA (widget), error);
      g_error_free (error);
      return;
    }

  /* initialize the vertex buffers */
  init_buffers (scene_info.vertex_pos_attrib, scene_info.texture_coord_attrib,
                &scene_info.indice_buffer, &scene_info.vertex_buffer);

  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&scene_info.draw_mutex);

  scene_info.gl_context = glXGetCurrentContext();
  scene_info.display = gdk_x11_display_get_xdisplay (gdk_display_get_default ());
}

static void
destroy (GtkWidget *widget)
{
  gst_element_set_state (GST_ELEMENT (scene_info.pipeline), GST_STATE_NULL);
  gst_object_unref (scene_info.pipeline);
  gtk_main_quit();
}

static void
unrealize (GtkWidget *widget)
{
  /* we need to ensure that the GdkGLContext is set before calling GL API */
  gtk_gl_area_make_current (GTK_GL_AREA (widget));

  /* skip everything if we're in error state */
  if (gtk_gl_area_get_error (GTK_GL_AREA (widget)) != NULL)
    return;

  /* destroy all the resources we created */
  if (scene_info.vertex_buffer != 0)
    glDeleteBuffers (1, &scene_info.vertex_buffer);
  if (scene_info.indice_buffer != 0)
    glDeleteBuffers (1, &scene_info.indice_buffer);
  if (scene_info.program != 0)
    glDeleteProgram (scene_info.program);
}

static void
unmap_texture_buffer_callback(TextureBuffer* buffer)
{
  gst_video_frame_unmap(&buffer->video_frame);
}

static void
free_texture_buffer_callback(TextureBuffer* buffer)
{
  g_free (buffer);
}

static gboolean
render (GtkGLArea *area, GdkGLContext *context)
{
  GstBuffer *inbuf = g_async_queue_try_pop (scene_info.queue_input_buf);

  GstVideoMeta *v_meta;
  GstVideoInfo info;
  GstVideoFrame frame;
  guint tex_id;

  if (!inbuf)
    return FALSE;

  v_meta = gst_buffer_get_video_meta (inbuf);
  if (!v_meta) {
    g_warning ("Required Meta was not found on buffers");
    return FALSE;
  }

  gst_video_info_set_format (&info, v_meta->format, v_meta->width,
      v_meta->height);

  if (!gst_video_frame_map (&frame, &info, inbuf, GST_MAP_READ | GST_MAP_GL)) {
    g_warning ("Failed to map video frame");
    return FALSE;
  }

  if (!gst_is_gl_memory (frame.map[0].memory)) {
    g_warning ("Input buffer does not have GLMemory");
    gst_video_frame_unmap (&frame);
    return FALSE;
  }

  tex_id = *(guint *) frame.data[0];

  // inside this function it's safe to use GL; the given
  // #GdkGLContext has been made current to the drawable
  // surface used by the #GtkGLArea and the viewport has
  // already been set to be the size of the allocation

  // we can start by clearing the buffer
  GLCOMMAND(glClearColor (0, 0, 0, 0));
  GLCOMMAND(glClear (GL_COLOR_BUFFER_BIT));

  if (scene_info.program == 0 || scene_info.vertex_buffer == 0)
    return TRUE;

  /* load our program */
  GLCOMMAND(glUseProgram (scene_info.program));

  GLCOMMAND(glBindBuffer (GL_ARRAY_BUFFER, scene_info.vertex_buffer));
  GLCOMMAND(glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, scene_info.indice_buffer));

  GLCOMMAND(glActiveTexture(GL_TEXTURE0));
  GLCOMMAND(glBindTexture(GL_TEXTURE_2D, tex_id));
  GLCOMMAND(glUniform1i (scene_info.texture_attrib, 0));

  /* draw the three vertices as a triangle */
  GLCOMMAND(glDrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0));

  /* we finished using the buffers and program */
  GLCOMMAND(glBindVertexArray (0));
  GLCOMMAND(glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0));
  GLCOMMAND(glUseProgram (0));

  // we completed our drawing; the draw commands will be
  // flushed at the end of the signal emission chain, and
  // the buffers will be drawn on the window

  gst_video_frame_unmap (&frame);
  g_async_queue_push (scene_info.queue_output_buf, inbuf);
  return TRUE;
}

static gboolean
ensure_gst_glcontext()
{
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&scene_info.draw_mutex);

  if (GST_IS_GL_CONTEXT(scene_info.gst_gl_context))
    return TRUE;

  scene_info.gst_gl_display = GST_GL_DISPLAY (gst_gl_display_x11_new_with_display (scene_info.display));
  GstGLPlatform gst_gl_platform = GST_GL_PLATFORM_GLX;
  GstGLAPI gst_gl_API = GST_GL_API_OPENGL;

  if (!scene_info.gl_context)
    return FALSE;

  scene_info.gst_gl_context = gst_gl_context_new_wrapped(scene_info.gst_gl_display, (guintptr) scene_info.gl_context, gst_gl_platform, gst_gl_API);
  return TRUE;
}

static GstBusSyncReply
handle_sync_message (GstBus * bus, GstMessage * message, gpointer userData)
{
  const gchar* context_type;

  if (GST_MESSAGE_TYPE (message) != GST_MESSAGE_NEED_CONTEXT)
    return GST_BUS_DROP;

  gst_message_parse_context_type(message, &context_type);

  if (!ensure_gst_glcontext())
    return GST_BUS_DROP;

  if (!g_strcmp0(context_type, GST_GL_DISPLAY_CONTEXT_TYPE)) {
    GstContext* display_context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
    gst_context_set_gl_display(display_context, scene_info.gst_gl_display);
    gst_element_set_context(GST_ELEMENT(message->src), display_context);
    return GST_BUS_DROP;
  }

  if (!g_strcmp0(context_type, "gst.gl.app_context")) {
      GstContext* app_context = gst_context_new("gst.gl.app_context", TRUE);
      GstStructure* structure = gst_context_writable_structure(app_context);
      gst_structure_set(structure, "context", GST_GL_TYPE_CONTEXT, scene_info.gst_gl_context, NULL);
      gst_element_set_context(GST_ELEMENT(message->src), app_context);
  }

  return GST_BUS_DROP;
}

/* fakesink handoff callback */
static void
on_gst_buffer (GstElement * fakesink, GstBuffer * buf, GstPad * pad,
    gpointer data)
{
  GAsyncQueue *queue_input_buf = NULL;
  GAsyncQueue *queue_output_buf = NULL;

  gst_buffer_ref (buf);
  queue_input_buf =
      (GAsyncQueue *) g_object_get_data (G_OBJECT (fakesink),
      "queue_input_buf");
  g_async_queue_push (queue_input_buf, buf);
  if (g_async_queue_length (queue_input_buf) > 3)
    gtk_gl_area_queue_render (GTK_GL_AREA (scene_info.gl_area));

  /* pop then unref buffer we have finished to use in sdl */
  queue_output_buf =
      (GAsyncQueue *) g_object_get_data (G_OBJECT (fakesink),
      "queue_output_buf");
  if (g_async_queue_length (queue_output_buf) > 3) {
    GstBuffer *buf_old = (GstBuffer *) g_async_queue_pop (queue_output_buf);
    gst_buffer_unref (buf_old);
  }
}

GstElement* createVideoSink()
{
  GstElement *videosink, *upload, *colorconvert, *fakesink;
  GstCaps *caps;
  GstPad *pad;

  videosink = gst_bin_new("gstglsinkbin");
  upload = gst_element_factory_make("glupload", NULL);
  colorconvert = gst_element_factory_make("glcolorconvert", NULL);
  fakesink = gst_element_factory_make("fakesink", NULL);

  gst_bin_add_many(GST_BIN(videosink), upload, colorconvert, fakesink, NULL);

  caps = gst_caps_from_string(GST_WEBKIT_VIDEO_CAPS);

  gst_element_link_pads(upload, "src", colorconvert, "sink");
  gst_element_link_pads_filtered(colorconvert, "src", fakesink, "sink", caps);
  gst_caps_unref(caps);

  pad = gst_element_get_static_pad(upload, "sink");
  gst_element_add_pad(videosink, gst_ghost_pad_new("sink", pad));
  g_object_unref(pad);

  pad = gst_element_get_static_pad(fakesink, "sink");
  g_object_set(fakesink, "enable-last-sample", FALSE, "signal-handoffs", TRUE, "silent", TRUE, "sync", TRUE, NULL);
  g_object_unref(pad);

  g_signal_connect (fakesink, "handoff", G_CALLBACK (on_gst_buffer), NULL);
  g_object_set_data (G_OBJECT (fakesink), "queue_input_buf", scene_info.queue_input_buf);
  g_object_set_data (G_OBJECT (fakesink), "queue_output_buf", scene_info.queue_output_buf);

  return videosink;
}

static unsigned getGstPlayFlag(const char* nick)
{
  static GFlagsClass* flagsClass = NULL;
  GFlagsValue* flag = NULL;

  if (!flagsClass)
    flagsClass = (GFlagsClass*)(g_type_class_ref(g_type_from_name("GstPlayFlags")));

  flag = g_flags_get_value_by_nick(flagsClass, nick);
  if (!flag)
      return 0;

  return flag->value;
}

static void
activate ()
{
  GtkWidget *window;
  GtkWidget *gl_area;
  GtkWidget *button_box;
  GstElement *playbin;
  GstStateChangeReturn ret;
  GstCaps *caps;
  GstBus *bus;

  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (window), "Window");
  gtk_window_set_default_size (GTK_WINDOW (window), 800, 600);

  gl_area = gtk_gl_area_new ();
  g_signal_connect (gl_area, "render", G_CALLBACK (render), NULL);
  g_signal_connect (gl_area, "realize", G_CALLBACK (realize), NULL);
  g_signal_connect (gl_area, "unrealize", G_CALLBACK (unrealize), NULL);
  gtk_gl_area_set_auto_render (GTK_GL_AREA (gl_area), FALSE);
  gtk_container_add (GTK_CONTAINER (window), gl_area);
  scene_info.gl_area = gl_area;

  gtk_widget_show_all (window);

  g_mutex_init (&scene_info.draw_mutex);
  scene_info.sample = NULL;
  scene_info.current_buffer = NULL;
  scene_info.pending_buffer = NULL;
  scene_info.queue_input_buf = g_async_queue_new ();
  scene_info.queue_output_buf = g_async_queue_new ();

  /* create elements */
  scene_info.pipeline = gst_pipeline_new ("pipeline");
  playbin = gst_element_factory_make ("playbin", "playbin");
  g_object_set(playbin, "video-sink", createVideoSink(), NULL);
  g_object_set(playbin, "audio-sink", gst_element_factory_make("autoaudiosink", 0), NULL);

  unsigned flagText = getGstPlayFlag("text");
  unsigned flagAudio = getGstPlayFlag("audio");
  unsigned flagVideo = getGstPlayFlag("video");
  unsigned flagNativeVideo = getGstPlayFlag("native-video");
  g_object_set(playbin, "flags", flagText | flagAudio | flagVideo | flagNativeVideo, NULL);

  gst_bin_add_many(GST_BIN(scene_info.pipeline), playbin, NULL);
  fprintf(stderr, "%s\n", scene_info.uri);
  g_object_set (G_OBJECT (playbin), "uri", scene_info.uri, NULL);

  bus = gst_pipeline_get_bus (GST_PIPELINE (scene_info.pipeline));
  gst_bus_set_sync_handler(bus, (GstBusSyncHandler) (handle_sync_message), NULL, NULL);
  gst_object_unref (bus);

  //start
  ret = gst_element_set_state (scene_info.pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_print ("Failed to start up pipeline!\n");
    return;
  }
}

int
main (int    argc,
      char **argv)
{
  GError* error;
  XInitThreads();
  gtk_init (&argc, &argv);
  gst_init (&argc, &argv);

  if (argc < 2) {
    g_print ("Usage: gstglview <uri-to-play>\n");
    return 1;
  }

  scene_info.uri = gst_filename_to_uri (argv[1], &error);

  activate();
  gtk_main ();
  gst_deinit ();

  g_free (scene_info.uri);

  return 0;
}

