// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <glad/glad.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPainter>
#include <QScreen>
#include <QStringList>
#include <QWindow>

#ifdef HAS_OPENGL
#include <QOffscreenSurface>
#include <QOpenGLContext>
#ifdef DOLPHIN
#ifdef _WIN32
#include "Common/GL/GLInterface/WGL.h"
#endif
#endif
#endif

#if !defined(WIN32) && HAS_VULKAN
#include <qpa/qplatformnativeinterface.h>
#endif

#include <fmt/format.h>

#include "common/assert.h"
#include "common/microprofile.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/settings.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
#include "input_common/motion_emu.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"
#include "yuzu/bootmanager.h"
#include "yuzu/main.h"

#ifdef DOLPHIN
#include "Common/GL/GLContext.h"
//#include "Common/GL/GLExtensions/GLExtensions.h"
#include "Common/WindowSystemInfo.h"
#include "DolphinQt/RenderWidget.h"
#include "VideoCommon/VideoConfig.h"
#undef RenderWidget

extern WindowSystemInfo g_wsi;
#endif

EmuThread::EmuThread() = default;

EmuThread::~EmuThread() = default;

void EmuThread::run() {
    std::string name = "yuzu:EmuControlThread";
    MicroProfileOnThreadCreate(name.c_str());
    Common::SetCurrentThreadName(name.c_str());

    auto& system = Core::System::GetInstance();

    system.RegisterHostThread();

    auto& gpu = system.GPU();

    // Main process has been loaded. Make the context current to this thread and begin GPU and CPU
    // execution.
    gpu.Start();

    gpu.ObtainContext();

    emit LoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0);

    system.Renderer().Rasterizer().LoadDiskResources(
        stop_run, [this](VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total) {
            emit LoadProgress(stage, value, total);
        });

    emit LoadProgress(VideoCore::LoadCallbackStage::Complete, 0, 0);

    gpu.ReleaseContext();

    // Holds whether the cpu was running during the last iteration,
    // so that the DebugModeLeft signal can be emitted before the
    // next execution step
    bool was_active = false;
    while (!stop_run) {
        if (running) {
            if (was_active) {
                emit DebugModeLeft();
            }

            running_guard = true;
            Core::System::ResultStatus result = system.Run();
            if (result != Core::System::ResultStatus::Success) {
                running_guard = false;
                this->SetRunning(false);
                emit ErrorThrown(result, system.GetStatusDetails());
            }
            running_wait.Wait();
            result = system.Pause();
            if (result != Core::System::ResultStatus::Success) {
                running_guard = false;
                this->SetRunning(false);
                emit ErrorThrown(result, system.GetStatusDetails());
            }
            running_guard = false;

            if (!stop_run) {
                was_active = true;
                emit DebugModeEntered();
            }
        } else if (exec_step) {
            UNIMPLEMENTED();
        } else {
            std::unique_lock lock{running_mutex};
            running_cv.wait(lock, [this] { return IsRunning() || exec_step || stop_run; });
        }
    }

    // Shutdown the core emulation
    system.Shutdown();

#if MICROPROFILE_ENABLED
    MicroProfileOnThreadExit();
#endif
}

#ifdef HAS_OPENGL
// QWGLNativeContext header file is missing, so this won't work
#if 0
std::unique_ptr<QOpenGLContext> GetDolphinContext(GLContext* dolphin_context) {
    if (!dolphin_context) {
        return nullptr;
    }
#ifdef _WIN32
    auto context = std::make_unique<QOpenGLContext>();
    GLContextWGL* w = (GLContextWGL*)dolphin_context;
    QWGLNativeContext native(w->m_rc, w->m_window_handle);
    context->setNativeHandle(native);
    if (!context->create()) {
        LOG_ERROR(Frontend, "Unable to create Dolphin openGL context");
    }
#else
    return nullptr;
#endif
}
#endif

class OpenGLSharedContext : public Core::Frontend::GraphicsContext {
public:
    /// Create the original context that should be shared from
    explicit OpenGLSharedContext(const WindowSystemInfo& wsi) : m_wsi(wsi) {
        auto old_context = g_first_context;
        m_context = GLContext::Create(wsi, false, false, false, false);
        if (!m_context) {
            LOG_ERROR(Frontend, "Unable to create main openGL context");
            return;
        }
        if (old_context) {
            old_context->ClearOtherThread();
            old_context->ShareWith(m_context.get());
            old_context->RestoreOtherThread();
        }

        // TODO: expose a setting for buffer value (ie default/single/double/triple)
        m_context->SwapInterval(0);
    }

    /// Create the shared contexts for rendering and presentation
    explicit OpenGLSharedContext(GLContext* share_context, WindowSystemInfo* main_wsi = nullptr) {
        if (main_wsi) {
            m_context = GLContext::Create(*main_wsi, false, false, false, false);
            if (!m_context) {
                LOG_ERROR(Frontend, "Unable to create shared openGL context");
                return;
            }
            share_context->ShareWith(m_context.get());
        } else {
            m_context = share_context->CreateSharedContext(false);
        }

        // disable vsync for any [windowless] shared contexts
        if (main_wsi) {
            m_context->SwapInterval(0);
            m_wsi = *main_wsi;
        }
    }

    ~OpenGLSharedContext() {
        DoneCurrent();
    }

    void SwapBuffers() override {
        m_context->Swap();
    }

    void MakeCurrent() override {
        // We can't track the current state of the underlying context in this wrapper class because
        // Qt may make the underlying context not current for one reason or another. In particular,
        // the WebBrowser uses GL, so it seems to conflict if we aren't careful.
        // Instead of always just making the context current (which does not have any caching to
        // check if the underlying context is already current) we can check for the current context
        // in the thread local data by calling `currentContext()` and checking if its ours.
        m_context->CheckThisThread();
    }

    void DoneCurrent() override {
        m_context->ClearCurrent();
    }

    GLContext* GetShareContext() {
        return m_context.get();
    }

private:
    std::unique_ptr<GLContext> m_context;
    WindowSystemInfo m_wsi;
};
#endif

class DummyContext : public Core::Frontend::GraphicsContext {};

class RenderWidget : public QWidget {
public:
    explicit RenderWidget(GRenderWindow* parent) : QWidget(parent), render_window(parent) {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
    }

    virtual ~RenderWidget() = default;

    /// Called on the UI thread when this Widget is ready to draw
    /// Dervied classes can override this to draw the latest frame.
    virtual void Present() {}

    void paintEvent(QPaintEvent* event) override {
        Present();
        update();
    }

    QPaintEngine* paintEngine() const override {
        return nullptr;
    }

private:
    GRenderWindow* render_window;
};

#ifdef HAS_VULKAN
class VulkanRenderWidget : public RenderWidget {
public:
    explicit VulkanRenderWidget(GRenderWindow* parent) : RenderWidget(parent) {
        windowHandle()->setSurfaceType(QWindow::VulkanSurface);
    }
};
#endif

static Core::Frontend::WindowSystemType GetWindowSystemType() {
    // Determine WSI type based on Qt platform.
    QString platform_name = QGuiApplication::platformName();
    if (platform_name == QStringLiteral("windows"))
        return Core::Frontend::WindowSystemType::Windows;
    else if (platform_name == QStringLiteral("xcb"))
        return Core::Frontend::WindowSystemType::X11;
    else if (platform_name == QStringLiteral("wayland"))
        return Core::Frontend::WindowSystemType::Wayland;

    LOG_CRITICAL(Frontend, "Unknown Qt platform!");
    return Core::Frontend::WindowSystemType::Windows;
}

#if 0
static Core::Frontend::EmuWindow::WindowSystemInfo GetWindowSystemInfo(QWindow* window) {
    Core::Frontend::EmuWindow::WindowSystemInfo wsi;
    wsi.type = GetWindowSystemType();

#ifdef HAS_VULKAN
    // Our Win32 Qt external doesn't have the private API.
#if defined(WIN32) || defined(__APPLE__)
    wsi.render_surface = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
#else
    QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
    wsi.display_connection = pni->nativeResourceForWindow("display", window);
    if (wsi.type == Core::Frontend::WindowSystemType::Wayland)
        wsi.render_surface = window ? pni->nativeResourceForWindow("surface", window) : nullptr;
    else
        wsi.render_surface = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
#endif
    wsi.render_surface_scale = window ? static_cast<float>(window->devicePixelRatio()) : 1.0f;
#endif

    return wsi;
}
#endif

GRenderWindow::GRenderWindow(GMainWindow* parent_, EmuThread* emu_thread_)
    : QWidget(parent_), emu_thread(emu_thread_) {
    setWindowTitle(QStringLiteral("yuzu %1 | %2-%3")
                       .arg(QString::fromUtf8(Common::g_build_name),
                            QString::fromUtf8(Common::g_scm_branch),
                            QString::fromUtf8(Common::g_scm_desc)));
    setAttribute(Qt::WA_AcceptTouchEvents);
    auto layout = new QHBoxLayout(this);
    layout->setMargin(0);
    setLayout(layout);
    InputCommon::Init();

    this->setMouseTracking(true);

    if (parent_)
      connect(this, &GRenderWindow::FirstFrameDisplayed, parent_, &GMainWindow::OnLoadComplete);
}

GRenderWindow::~GRenderWindow() {
    InputCommon::Shutdown();
}

void GRenderWindow::PollEvents() {
    if (!first_frame) {
        first_frame = true;
        emit FirstFrameDisplayed();
    }
}

bool GRenderWindow::IsShown() const {
    return !isMinimized();
}

// On Qt 5.0+, this correctly gets the size of the framebuffer (pixels).
//
// Older versions get the window size (density independent pixels),
// and hence, do not support DPI scaling ("retina" displays).
// The result will be a viewport that is smaller than the extent of the window.
void GRenderWindow::OnFramebufferSizeChanged() {
    // Screen changes potentially incur a change in screen DPI, hence we should update the
    // framebuffer size
    const qreal pixel_ratio = windowPixelRatio();
    const u32 width = this->width() * pixel_ratio;
    const u32 height = this->height() * pixel_ratio;
    UpdateCurrentFramebufferLayout(width, height);
}

void GRenderWindow::BackupGeometry() {
    geometry = QWidget::saveGeometry();
}

void GRenderWindow::RestoreGeometry() {
    // We don't want to back up the geometry here (obviously)
    QWidget::restoreGeometry(geometry);
}

void GRenderWindow::restoreGeometry(const QByteArray& geometry) {
    // Make sure users of this class don't need to deal with backing up the geometry themselves
    QWidget::restoreGeometry(geometry);
    BackupGeometry();
}

QByteArray GRenderWindow::saveGeometry() {
    // If we are a top-level widget, store the current geometry
    // otherwise, store the last backup
    if (parent() == nullptr) {
        return QWidget::saveGeometry();
    }

    return geometry;
}

qreal GRenderWindow::windowPixelRatio() const {
    return devicePixelRatioF();
}

std::pair<u32, u32> GRenderWindow::ScaleTouch(const QPointF& pos) const {
    const qreal pixel_ratio = windowPixelRatio();
    return {static_cast<u32>(std::max(std::round(pos.x() * pixel_ratio), qreal{0.0})),
            static_cast<u32>(std::max(std::round(pos.y() * pixel_ratio), qreal{0.0}))};
}

void GRenderWindow::closeEvent(QCloseEvent* event) {
    emit Closed();
    QWidget::closeEvent(event);
}

void GRenderWindow::keyPressEvent(QKeyEvent* event) {
    InputCommon::GetKeyboard()->PressKey(event->key());
}

void GRenderWindow::keyReleaseEvent(QKeyEvent* event) {
    InputCommon::GetKeyboard()->ReleaseKey(event->key());
}

void GRenderWindow::mousePressEvent(QMouseEvent* event) {
    // touch input is handled in TouchBeginEvent
    if (event->source() == Qt::MouseEventSynthesizedBySystem) {
        return;
    }

    auto pos = event->pos();
    if (event->button() == Qt::LeftButton) {
        const auto [x, y] = ScaleTouch(pos);
        this->TouchPressed(x, y);
    } else if (event->button() == Qt::RightButton) {
        InputCommon::GetMotionEmu()->BeginTilt(pos.x(), pos.y());
    }
    QWidget::mousePressEvent(event);
}

void GRenderWindow::mouseMoveEvent(QMouseEvent* event) {
    // touch input is handled in TouchUpdateEvent
    if (event->source() == Qt::MouseEventSynthesizedBySystem) {
        return;
    }

    auto pos = event->pos();
    const auto [x, y] = ScaleTouch(pos);
    this->TouchMoved(x, y);
    InputCommon::GetMotionEmu()->Tilt(pos.x(), pos.y());
    QWidget::mouseMoveEvent(event);
}

void GRenderWindow::mouseReleaseEvent(QMouseEvent* event) {
    // touch input is handled in TouchEndEvent
    if (event->source() == Qt::MouseEventSynthesizedBySystem) {
        return;
    }

    if (event->button() == Qt::LeftButton) {
        this->TouchReleased();
    } else if (event->button() == Qt::RightButton) {
        InputCommon::GetMotionEmu()->EndTilt();
    }
}

void GRenderWindow::TouchBeginEvent(const QTouchEvent* event) {
    // TouchBegin always has exactly one touch point, so take the .first()
    const auto [x, y] = ScaleTouch(event->touchPoints().first().pos());
    this->TouchPressed(x, y);
}

void GRenderWindow::TouchUpdateEvent(const QTouchEvent* event) {
    QPointF pos;
    int active_points = 0;

    // average all active touch points
    for (const auto tp : event->touchPoints()) {
        if (tp.state() & (Qt::TouchPointPressed | Qt::TouchPointMoved | Qt::TouchPointStationary)) {
            active_points++;
            pos += tp.pos();
        }
    }

    pos /= active_points;

    const auto [x, y] = ScaleTouch(pos);
    this->TouchMoved(x, y);
}

void GRenderWindow::TouchEndEvent() {
    this->TouchReleased();
}

bool GRenderWindow::event(QEvent* event) {
    if (event->type() == QEvent::TouchBegin) {
        TouchBeginEvent(static_cast<QTouchEvent*>(event));
        return true;
    } else if (event->type() == QEvent::TouchUpdate) {
        TouchUpdateEvent(static_cast<QTouchEvent*>(event));
        return true;
    } else if (event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel) {
        TouchEndEvent();
        return true;
    }

    return QWidget::event(event);
}

void GRenderWindow::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    InputCommon::GetKeyboard()->ReleaseAllKeys();
}

void GRenderWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    OnFramebufferSizeChanged();
}

std::unique_ptr<Core::Frontend::GraphicsContext> GRenderWindow::CreateSharedContext() const {
#ifdef HAS_OPENGL
    if (Settings::values.renderer_backend == Settings::RendererBackend::OpenGL) {
        auto c = static_cast<OpenGLSharedContext*>(main_context.get());
        // Bind the shared contexts to the main surface in case the backend wants to take over
        // presentation
        return std::make_unique<OpenGLSharedContext>(c->GetShareContext(), &g_wsi);
    }
#endif
    return std::make_unique<DummyContext>();
}

bool GRenderWindow::InitRenderTarget() {
    ReleaseRenderTarget();

    first_frame = false;

    switch (Settings::values.renderer_backend) {
    case Settings::RendererBackend::OpenGL:
        if (!InitializeOpenGL()) {
            return false;
        }
        break;
    case Settings::RendererBackend::Vulkan:
        if (!InitializeVulkan()) {
            return false;
        }
        break;
    }

#if 0
    // Update the Window System information with the new render target
    window_info = GetWindowSystemInfo(child_widget->windowHandle());

    child_widget->resize(Layout::ScreenUndocked::Width, Layout::ScreenUndocked::Height);
    layout()->addWidget(child_widget);
#endif
    // Reset minimum required size to avoid resizing issues on the main window after restarting.
    setMinimumSize(1, 1);

    resize(Layout::ScreenUndocked::Width, Layout::ScreenUndocked::Height);

    OnMinimalClientAreaChangeRequest(GetActiveConfig().min_client_area_size);
    OnFramebufferSizeChanged();
    BackupGeometry();

    if (Settings::values.renderer_backend == Settings::RendererBackend::OpenGL) {
        if (!LoadOpenGL()) {
            return false;
        }
    }

    return true;
}

void GRenderWindow::ReleaseRenderTarget() {
    child_context.reset();
    main_context.reset();
}

void GRenderWindow::CaptureScreenshot(u32 res_scale, const QString& screenshot_path) {
    auto& renderer = Core::System::GetInstance().Renderer();

    if (res_scale == 0) {
        res_scale = VideoCore::GetResolutionScaleFactor(renderer);
    }

    const Layout::FramebufferLayout layout{Layout::FrameLayoutFromResolutionScale(res_scale)};
    screenshot_image = QImage(QSize(layout.width, layout.height), QImage::Format_RGB32);
    renderer.RequestScreenshot(
        screenshot_image.bits(),
        [=] {
            const std::string std_screenshot_path = screenshot_path.toStdString();
            if (screenshot_image.mirrored(false, true).save(screenshot_path)) {
                LOG_INFO(Frontend, "Screenshot saved to \"{}\"", std_screenshot_path);
            } else {
                LOG_ERROR(Frontend, "Failed to save screenshot to \"{}\"", std_screenshot_path);
            }
        },
        layout);
}

void GRenderWindow::OnMinimalClientAreaChangeRequest(std::pair<u32, u32> minimal_size) {
    setMinimumSize(minimal_size.first, minimal_size.second);
}

bool GRenderWindow::InitializeOpenGL() {
#ifdef HAS_OPENGL
    GLContext* oldContext = g_first_context;
    std::unique_ptr<GLContext> main_gl_context = NULL;
    bool prefer_gles = false; // Config::Get(Config::GFX_PREFER_GLES)
    main_gl_context = GLContext::Create(g_wsi, g_Config.stereo_mode == StereoMode::QuadBuffer,
                                        false, false, prefer_gles);
    if (oldContext) {
        oldContext->ClearOtherThread();
        oldContext->ShareWith(main_gl_context.get());
        oldContext->RestoreOtherThread();
    } else {
        if (!main_gl_context)
            return false;
        // if (!GLExtensions::Init(main_gl_context.get()))
        //    return false;
    }
    std::unique_ptr<GLContext> child_gl_context = GLContext::Create(
        g_wsi, g_Config.stereo_mode == StereoMode::QuadBuffer, false, false, prefer_gles);
    main_gl_context->ShareWith(child_gl_context.get());

    auto context = std::make_shared<OpenGLSharedContext>(g_wsi);
    // child_context is used for drawing the main window from a different thread
    child_context = std::make_unique<OpenGLSharedContext>(context->GetShareContext(), &g_wsi);
    main_context = context;

    return true;
#else
    QMessageBox::warning(this, tr("OpenGL not available!"),
                         tr("yuzu has not been compiled with OpenGL support."));
    return false;
#endif
}

bool GRenderWindow::InitializeVulkan() {
#ifdef HAS_VULKAN
    //auto child = new VulkanRenderWidget(this);
    //child_widget = child;
    //child_widget->windowHandle()->create();
    main_context = std::make_unique<DummyContext>();

    return true;
#else
    QMessageBox::critical(this, tr("Vulkan not available!"),
                          tr("yuzu has not been compiled with Vulkan support."));
    return false;
#endif
}

bool GRenderWindow::LoadOpenGL() {
    auto context = CreateSharedContext();
    auto scope = context->Acquire();
    if (!gladLoadGL()) {
        QMessageBox::critical(this, tr("Error while initializing OpenGL 4.3!"),
                              tr("Your GPU may not support OpenGL 4.3, or you do not have the "
                                 "latest graphics driver."));
        return false;
    }

    QStringList unsupported_gl_extensions = GetUnsupportedGLExtensions();
    if (!unsupported_gl_extensions.empty()) {
        QMessageBox::critical(
            this, tr("Error while initializing OpenGL!"),
            tr("Your GPU may not support one or more required OpenGL extensions. Please ensure you "
               "have the latest graphics driver.<br><br>Unsupported extensions:<br>") +
                unsupported_gl_extensions.join(QStringLiteral("<br>")));
        return false;
    }
    return true;
}

QStringList GRenderWindow::GetUnsupportedGLExtensions() const {
    QStringList unsupported_ext;

    if (!GLAD_GL_ARB_buffer_storage)
        unsupported_ext.append(QStringLiteral("ARB_buffer_storage"));
    if (!GLAD_GL_ARB_direct_state_access)
        unsupported_ext.append(QStringLiteral("ARB_direct_state_access"));
    if (!GLAD_GL_ARB_vertex_type_10f_11f_11f_rev)
        unsupported_ext.append(QStringLiteral("ARB_vertex_type_10f_11f_11f_rev"));
    if (!GLAD_GL_ARB_texture_mirror_clamp_to_edge)
        unsupported_ext.append(QStringLiteral("ARB_texture_mirror_clamp_to_edge"));
    if (!GLAD_GL_ARB_multi_bind)
        unsupported_ext.append(QStringLiteral("ARB_multi_bind"));
    if (!GLAD_GL_ARB_clip_control)
        unsupported_ext.append(QStringLiteral("ARB_clip_control"));

    // Extensions required to support some texture formats.
    if (!GLAD_GL_EXT_texture_compression_s3tc)
        unsupported_ext.append(QStringLiteral("EXT_texture_compression_s3tc"));
    if (!GLAD_GL_ARB_texture_compression_rgtc)
        unsupported_ext.append(QStringLiteral("ARB_texture_compression_rgtc"));
    if (!GLAD_GL_ARB_depth_buffer_float)
        unsupported_ext.append(QStringLiteral("ARB_depth_buffer_float"));

    for (const QString& ext : unsupported_ext)
        LOG_CRITICAL(Frontend, "Unsupported GL extension: {}", ext.toStdString());

    return unsupported_ext;
}

void GRenderWindow::OnEmulationStarting(EmuThread* emu_thread) {
    this->emu_thread = emu_thread;
}

void GRenderWindow::OnEmulationStopping() {
    emu_thread = nullptr;
}

void GRenderWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);

    // windowHandle() is not initialized until the Window is shown, so we connect it here.
    connect(windowHandle(), &QWindow::screenChanged, this, &GRenderWindow::OnFramebufferSizeChanged,
            Qt::UniqueConnection);
}
