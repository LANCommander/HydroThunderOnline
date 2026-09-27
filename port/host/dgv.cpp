/* dgVoodoo.conf writer on top of dgVoodoo's API library; see dgv.h. */
#include <windows.h>

#include "APIDll/IMainFactory.hpp"
#include "IAPIDataStream.hpp"

extern "C" {
#pragma warning(push)
#pragma warning(disable : 26812) /* C interface: its enums can't be enum class (Enum.3) */
#include "dgv.h"
#include "host.h"
#pragma warning(pop)
}

#include "cpp_util.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace {

using PfnApiInit = IMainFactory *(*)();
using PfnApiGetVersion = UInt32 (*)();

/* Routes the API's debug layer into hydro.log. */
class LogStream final : public dgVoodoo::IAPIDataStream {
public:
    Status Seek(Int32, Origin, UInt32 *) const noexcept override { return StatusError; }
    Status Read(UInt32, void *, UInt32 *) const noexcept override { return StatusError; }
    UInt32 GetSize() const noexcept override { return 0; }

    Status Write(UInt32 count, void *buffer, UInt32 *written) const noexcept override
    {
        if (!buffer)
            return StatusError;

        std::string_view line(static_cast<const char *>(buffer), count);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.remove_suffix(1);

        if (!line.empty()) {
            const auto shown = std::min<std::size_t>(line.size(), 1024);
            hy_log("dgVoodooAPI: %.*s", hy::narrow_cast<int>(shown), line.data());
        }

        if (written)
            *written = count;
        return StatusOk;
    }
};

/* The folder hydro.exe is in, with a trailing backslash. */
std::string exe_dir()
{
    std::string path(MAX_PATH, '\0');
    for (;;) {
        const DWORD n = GetModuleFileNameA(nullptr, path.data(), hy::narrow<DWORD>(path.size()));
        if (n == 0)
            return {};

        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2); // truncated: try a bigger buffer
    }

    const auto slash = path.find_last_of('\\');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash + 1);
}

/* GetProcAddress returns a generic function pointer; the export's real type has to be asserted. */
template <typename Fn>
[[gsl::suppress("type.1")]] Fn proc(HMODULE m, const char *name) noexcept
{
    return reinterpret_cast<Fn>(GetProcAddress(m, name));
}

IMainFactory *load_factory()
{
    const std::string path = exe_dir() + "dgVoodooAPI.dll";
    HMODULE m = LoadLibraryA(path.c_str());
    if (!m) {
        hy_log("dgv: %s not found; graphics settings can't be applied", path.c_str());
        return nullptr;
    }

    const auto version = proc<PfnApiGetVersion>(m, "dgVoodoo_API_GetVersion");
    const auto init = proc<PfnApiInit>(m, "dgVoodoo_API_Init");
    if (!init) {
        hy_log("dgv: dgVoodoo_API_Init missing");
        return nullptr;
    }

    IMainFactory *f = init();
    hy_log("dgv: API library %x loaded%s", version ? version() : 0u, f ? "" : " but init failed");
    return f;
}

/* Loaded once (thread-safe static initialization); the library stays loaded for the life of the process. */
IMainFactory *factory()
{
    static IMainFactory *const f = load_factory();
    return f;
}

dgVoodoo::ConfigGeneral::ScalingMode scaling_mode(int scaling)
{
    using SM = dgVoodoo::ConfigGeneral;
    constexpr std::array<SM::ScalingMode, DGV_SCALE_COUNT> modes{
        SM::SM_Stretched, SM::SM_AspectRatio,         SM::SM_AspectRatio4_3,
        SM::SM_Centered,  SM::SM_CenteredAspectRatio, SM::SM_AspectRatio4_3_CRTLike,
    };

    if (scaling < 0 || scaling >= DGV_SCALE_COUNT)
        return modes.at(0);

    return modes.at(hy::narrow<std::size_t>(scaling));
}

constexpr dgVoodoo::ConfigGeneralExt::Resampling resampling(int r) noexcept
{
    using dgVoodoo::ConfigGeneralExt;
    switch (r) {
    case 0:
        return ConfigGeneralExt::RS_PointSampled;
    case 2:
        return ConfigGeneralExt::RS_Bicubic;
    case 3:
        return ConfigGeneralExt::RS_Lanczos_2;
    case 4:
        return ConfigGeneralExt::RS_Lanczos_3;
    default:
        return ConfigGeneralExt::RS_Bilinear;
    }
}

/* The resolution fields hold either a size or one of these special values. */
[[gsl::suppress("type.1")]] constexpr UInt32 special(dgVoodoo::ConfigGeneral::Resolution r) noexcept
{
    return static_cast<UInt32>(r);
}

void set_resolution(dgVoodoo::Config &c, const DgvGraphics &g)
{
    using dgVoodoo::ConfigGeneral;

    switch (g.resolution) {
    case DGV_RES_2X:
    case DGV_RES_3X:
    case DGV_RES_4X:
    case DGV_RES_5X:
    case DGV_RES_6X:
        c.glide.resWidth = special(ConfigGeneral::R_IntegerScaled);
        c.glide.resHeight = hy::narrow<UInt32>(2 + (g.resolution - DGV_RES_2X)); // the scale factor
        break;

    case DGV_RES_MAX:
        c.glide.resWidth = c.glide.resHeight = special(ConfigGeneral::R_Max);
        break;

    case DGV_RES_MAX_INTEGER:
        c.glide.resWidth = c.glide.resHeight = special(ConfigGeneral::R_Max_ISF);
        break;

    case DGV_RES_DESKTOP:
        /* "desktop" renders at the desktop's own shape (16:9 on most screens), which leaves the
         * scaling mode nothing to do: the picture always fills the screen. Unless stretching is
         * what the user picked, use "max" instead: the largest size with the game's 4:3 shape. */
        c.glide.resWidth = c.glide.resHeight =
            special(g.scaling == DGV_SCALE_STRETCHED ? ConfigGeneral::R_Desktop : ConfigGeneral::R_Max);
        break;

    default:
        c.glide.resWidth = c.glide.resHeight = special(ConfigGeneral::R_Unforced);
        break;
    }
}

constexpr dgVoodoo::ConfigGlide::TexFilterType tex_filter(int filter) noexcept
{
    using dgVoodoo::ConfigGlide;
    switch (filter) {
    case 1:
        return ConfigGlide::TF_ForcePoint;
    case 2:
        return ConfigGlide::TF_ForceBilinear;
    default:
        return ConfigGlide::TF_AppDriven;
    }
}

/* Settings the menu keeps in range; anything else (a hand-edited hydro.ini) falls back to neutral. */
UInt32 unsigned_or(int v, UInt32 fallback)
{
    return v >= 0 ? hy::narrow<UInt32>(v) : fallback;
}

/* Copies the settings the port manages into c. */
void apply(dgVoodoo::Config &c, const DgvGraphics &g)
{
    c.general.windowed = g.windowed != 0;
    c.general.scalingMode = scaling_mode(g.scaling);
    c.general.brightnessScale = unsigned_or(g.brightness, 100);
    c.general.contrastScale = unsigned_or(g.contrast, 100);
    c.general.colorScale = unsigned_or(g.color, 100);
    c.generalExt.resampling = resampling(g.resampling);

    set_resolution(c, g);
    c.glide.msaaLevel = unsigned_or(g.msaa, 0);
    c.glide.texFilterType = tex_filter(g.filter);
    c.glide.forceVSync = g.vsync != 0;

    /* The port has always run without these; the API's defaults turn them on. */
    c.glide.enable3DfxWaterMark = false;
    c.glide.enableSplashScreen = false;
}

bool write_conf(const DgvGraphics &g, const char *conf_path)
{
    using namespace dgVoodoo;

    IMainFactory *f = factory();
    IConfig *ic = f ? f->GetIConfig() : nullptr;
    if (!ic)
        return false;

    static LogStream log; // the API takes a non-const stream
    const APIDebugObj dbg(APIDebugObj::DisableInfo, APIDebugObj::EnableWarning, APIDebugObj::EnableError, "", 0,
                          &log);

    /* Start from the existing file so settings the port doesn't manage survive. */
    Config c;
    const bool exists = GetFileAttributesA(conf_path) != INVALID_FILE_ATTRIBUTES;
    if (exists && !ic->ReadConfig(c, conf_path, &dbg)) {
        hy_log("dgv: %s unreadable; rewriting it from defaults", conf_path);
        c = Config();
    }

    apply(c, g);

    if (!ic->ValidateConfig(c, &dbg))
        hy_log("dgv: config failed validation; writing it anyway");

    if (!ic->WriteConfig(c, conf_path, ic->GetINITemplate(), &dbg)) {
        hy_log("dgv: writing %s failed", conf_path);
        return false;
    }

    hy_log("dgv: wrote %s (%s, res %d, scaling %d, msaa %d, filter %d, vsync %d)", conf_path,
           g.windowed ? "windowed" : "fullscreen", g.resolution, g.scaling, g.msaa, g.filter, g.vsync);
    return true;
}

} // namespace

extern "C" int dgv_write_conf(const DgvGraphics *g, const char *conf_path)
{
    if (!g || !conf_path)
        return 0;

    try {
        return write_conf(*g, conf_path) ? 1 : 0;
    } catch (const std::exception &e) {
        hy_log("dgv: %s", e.what());
        return 0;
    }
}
