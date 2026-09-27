/* Player tracking markers 9-16: an HT.R2-format overlay built at load; see markers.h. */
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

extern "C" {
#include "host.h"
#include "markers.h"
}

#include "cpp_util.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

// R2 archive layout
//
// Header (0x88 bytes): the magic string, then the directory offset, data start and entry count.
// Directory entry (0x4c bytes): record offset, body size, reference count, 12-byte name, then fields we copy as is.
// Record: u32 (body size | type << 24), the body, u32 reference count,
//         16 bytes per reference (12-byte name, u32 body offset), and the object's own 12-byte name.
constexpr std::string_view kMagic{"James Cameron Rules"};

constexpr std::size_t kHeaderSize = 0x88;
constexpr std::size_t kHeaderDirOffset = 0x20;
constexpr std::size_t kHeaderDataStart = 0x24;
constexpr std::size_t kHeaderCount = 0x2c;

constexpr std::size_t kDirEntrySize = 0x4c;
constexpr std::size_t kEntryOffset = 0;
constexpr std::size_t kEntryBodySize = 4;
constexpr std::size_t kEntryRefCount = 8;
constexpr std::size_t kEntryName = 12;

constexpr std::size_t kNameSize = 12;
constexpr std::size_t kRefSize = 16;

constexpr std::uint32_t kMaxEntries = 0x100000;
constexpr std::uint32_t kMaxBodySize = 0x800000;
constexpr std::uint32_t kMaxRefs = 0x3fff;

// Texture body: nine u32s (slot, palette size, bytes per texel, format, width, height, largest LOD, smallest LOD,
// aspect), then the texels.
constexpr std::size_t kTexPaletteSize = 4;
constexpr std::size_t kTexBytesPerTexel = 8;
constexpr std::size_t kTexFormat = 12;
constexpr std::size_t kTexWidth = 16;
constexpr std::size_t kTexHeight = 20;
constexpr std::size_t kTexLodLarge = 24;
constexpr std::size_t kTexLodSmall = 28;
constexpr std::size_t kTexelOffset = 0x24;
constexpr std::uint32_t kFormatArgb4444 = 12;

// HT.R2's marker 8: every new marker copies its mesh and texture, swapping in new texels.
constexpr std::string_view kTemplateMesh{"GHWPLAYIDH8"};
constexpr std::string_view kTemplateTexture{"THWPLAYID28"};

constexpr int kFirstMarker = 9;
constexpr int kLastMarker = 16;

// ---- Byte helpers ----

constexpr std::size_t widen(std::uint32_t v) noexcept
{
    return v;
}

constexpr bool fits(std::size_t size, std::size_t at, std::size_t n) noexcept
{
    return at <= size && size - at >= n;
}

std::uint32_t read_u32(ByteView b, std::size_t at)
{
    std::uint32_t v{};
    if (!fits(b.size(), at, sizeof v))
        throw std::out_of_range("read_u32");

    std::memcpy(&v, b.subspan(at).data(), sizeof v);
    return v;
}

void write_u32(std::span<std::uint8_t> b, std::size_t at, std::uint32_t v)
{
    if (!fits(b.size(), at, sizeof v))
        throw std::out_of_range("write_u32");

    std::memcpy(b.subspan(at).data(), &v, sizeof v);
}

void append_u32(Bytes &b, std::uint32_t v)
{
    const auto at = b.size();
    b.resize(at + sizeof v);
    write_u32(b, at, v);
}

// A name field is 12 bytes, zero-padded; names are at most 11 characters.
void append_name(Bytes &b, std::string_view name)
{
    const auto n = std::min(name.size(), kNameSize - 1);
    for (const char c : name.substr(0, n))
        b.push_back(std::bit_cast<std::uint8_t>(c));

    b.resize(b.size() + kNameSize - n);
}

std::string read_name(ByteView b, std::size_t at)
{
    if (!fits(b.size(), at, kNameSize))
        throw std::out_of_range("read_name");

    std::string s;
    for (const auto c : b.subspan(at, kNameSize)) {
        if (c == 0)
            break;
        s.push_back(std::bit_cast<char>(c));
    }
    return s;
}

std::string upper(std::string_view s)
{
    std::string out(s);
    for (char &c : out) {
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
    }
    return out;
}

// "GHWPLAYID" + 9 -> "GHWPLAYID09"
std::string marker_name(std::string_view prefix, int n)
{
    const std::string digits = std::to_string(n);
    return std::string(prefix) + (digits.size() < 2 ? "0" : "") + digits;
}

// ---- Records ----

struct Ref {
    std::string name;
    std::uint32_t offset;
};

struct Record {
    std::uint8_t type;
    Bytes body;
    std::vector<Ref> refs;

    Bytes serialize(std::string_view name) const
    {
        Bytes out;

        append_u32(out, hy::narrow<std::uint32_t>(body.size()) | (std::uint32_t{type} << 24));
        out.insert(out.end(), body.begin(), body.end());

        append_u32(out, hy::narrow<std::uint32_t>(refs.size()));
        for (const auto &r : refs) {
            append_name(out, r.name);
            append_u32(out, r.offset);
        }

        append_name(out, name);
        return out;
    }
};

struct FileCloser {
    void operator()(std::FILE *f) const noexcept { std::fclose(f); }
};

using File = std::unique_ptr<std::FILE, FileCloser>;

// Read-only view of an R2 archive's header, directory and records.
class R2Archive {
public:
    static std::optional<R2Archive> open(const std::string &path)
    {
        R2Archive a;
        a.file_.reset(std::fopen(path.c_str(), "rb"));
        if (!a.file_)
            return std::nullopt;

        if (!a.read_at(0, kHeaderSize, a.header_) || !has_magic(a.header_))
            return std::nullopt;

        const auto dir_offset = read_u32(a.header_, kHeaderDirOffset);
        const auto count = read_u32(a.header_, kHeaderCount);
        if (count > kMaxEntries)
            return std::nullopt;

        Bytes dir;
        if (!a.read_at(dir_offset, widen(count) * kDirEntrySize, dir))
            return std::nullopt;

        const ByteView entries(dir);
        for (std::size_t i = 0; i < count; i++) {
            const auto e = entries.subspan(i * kDirEntrySize, kDirEntrySize);
            a.dir_.emplace(upper(read_name(e, kEntryName)), Bytes(e.begin(), e.end()));
        }
        return a;
    }

    const Bytes &header() const noexcept { return header_; }

    const Bytes *dir_entry(std::string_view name) const
    {
        const auto it = dir_.find(upper(name));
        return it == dir_.end() ? nullptr : &it->second;
    }

    std::optional<Record> read(std::string_view name)
    {
        const Bytes *e = dir_entry(name);
        if (!e)
            return std::nullopt;

        const auto offset = read_u32(*e, kEntryOffset);
        const auto size = read_u32(*e, kEntryBodySize);
        const auto nrefs = read_u32(*e, kEntryRefCount);
        if (size > kMaxBodySize || nrefs > kMaxRefs)
            return std::nullopt;

        const auto refs_at = 4 + widen(size) + 4;
        const auto record_size = refs_at + widen(nrefs) * kRefSize + kNameSize;
        Bytes b;
        if (!read_at(offset, record_size, b))
            return std::nullopt;

        // The record repeats the directory's body size and reference count.
        const auto head = read_u32(b, 0);
        if ((head & 0xffffff) != size || read_u32(b, refs_at - 4) != nrefs)
            return std::nullopt;

        const auto body = ByteView(b).subspan(4, size);
        Record r{hy::narrow<std::uint8_t>(head >> 24), Bytes(body.begin(), body.end()), {}};

        for (std::size_t i = 0; i < nrefs; i++) {
            const auto at = refs_at + i * kRefSize;
            r.refs.push_back({read_name(b, at), read_u32(b, at + kNameSize)});
        }
        return r;
    }

private:
    static bool has_magic(const Bytes &header)
    {
        const auto same = [](char m, std::uint8_t h) { return std::bit_cast<std::uint8_t>(m) == h; };
        return std::equal(kMagic.begin(), kMagic.end(), header.begin(), same);
    }

    bool read_at(std::uint32_t offset, std::size_t size, Bytes &out)
    {
        out.resize(size);
        if (_fseeki64(file_.get(), offset, SEEK_SET) != 0)
            return false;

        return std::fread(out.data(), 1, size, file_.get()) == size;
    }

    File file_;
    Bytes header_;
    std::map<std::string, Bytes> dir_; // upper-case name -> raw directory entry
};

// ---- Marker texture and mesh ----

struct TextureSize {
    std::uint32_t width;
    std::uint32_t height;
};

// Marker textures are single-LOD ARGB4444, stored bottom-up.
std::optional<TextureSize> marker_texture_size(const Bytes &body)
{
    if (body.size() < kTexelOffset)
        return std::nullopt;

    const bool argb4444 = read_u32(body, kTexPaletteSize) == 0 && read_u32(body, kTexBytesPerTexel) == 2 &&
                          read_u32(body, kTexFormat) == kFormatArgb4444;
    const bool one_lod = read_u32(body, kTexLodLarge) == read_u32(body, kTexLodSmall);
    if (!argb4444 || !one_lod)
        return std::nullopt;

    const TextureSize s{read_u32(body, kTexWidth), read_u32(body, kTexHeight)};
    if (s.width > 256 || s.height > 256)
        return std::nullopt;

    const auto texel_bytes = widen(s.width) * s.height * 2;
    if (body.size() - kTexelOffset < texel_bytes)
        return std::nullopt;

    return s;
}

// The one named reference of the marker mesh is its texture.
std::optional<std::size_t> texture_ref(const Record &mesh) noexcept
{
    std::optional<std::size_t> found;
    std::size_t i = 0;

    for (const auto &r : mesh.refs) {
        if (!r.name.empty()) {
            if (found)
                return std::nullopt; // more than one
            found = i;
        }
        i++;
    }
    return found;
}

// ---- PNG decoding (WIC) ----

class ComScope {
public:
    ComScope() noexcept : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

    ~ComScope()
    {
        if (hy::succeeded(hr_))
            CoUninitialize();
    }

    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;
    ComScope(ComScope &&) = delete;
    ComScope &operator=(ComScope &&) = delete;

    bool usable() const noexcept { return hy::succeeded(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

std::optional<Bytes> load_resource(int id)
{
    HMODULE self = GetModuleHandleW(nullptr);
    const std::wstring name = L"#" + std::to_wstring(id);

    HRSRC res = FindResourceW(self, name.c_str(), L"#10" /* RT_RCDATA */);
    if (!res)
        return std::nullopt;

    HGLOBAL mem = LoadResource(self, res);
    if (!mem)
        return std::nullopt;

    const auto *data = static_cast<const std::uint8_t *>(LockResource(mem));
    if (!data)
        return std::nullopt;

    const ByteView view(data, SizeofResource(self, res));
    return Bytes(view.begin(), view.end());
}

// Decodes a PNG of exactly the given size to 32-bit BGRA, top row first.
std::optional<Bytes> decode_bgra(IWICImagingFactory &wic, Bytes png, TextureSize size)
{
    ComPtr<IWICStream> stream;
    if (hy::failed(wic.CreateStream(&stream)))
        return std::nullopt;
    if (hy::failed(stream->InitializeFromMemory(png.data(), hy::narrow<DWORD>(png.size()))))
        return std::nullopt;

    ComPtr<IWICBitmapDecoder> decoder;
    if (hy::failed(wic.CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
        return std::nullopt;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (hy::failed(decoder->GetFrame(0, &frame)))
        return std::nullopt;

    UINT w = 0;
    UINT h = 0;
    if (hy::failed(frame->GetSize(&w, &h)) || w != size.width || h != size.height)
        return std::nullopt;

    ComPtr<IWICFormatConverter> converter;
    if (hy::failed(wic.CreateFormatConverter(&converter)))
        return std::nullopt;
    if (hy::failed(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                         0.0, WICBitmapPaletteTypeCustom)))
        return std::nullopt;

    const UINT stride = w * 4;
    Bytes bgra(widen(stride) * h);
    if (hy::failed(converter->CopyPixels(nullptr, stride, hy::narrow<UINT>(bgra.size()), bgra.data())))
        return std::nullopt;

    return bgra;
}

// 8 bits per channel to 4, rounded.
constexpr std::uint32_t to_4bit(std::uint8_t v) noexcept
{
    return (v * 15u + 127u) / 255u;
}

// BGRA, top row first -> ARGB4444 texels, bottom row first (the texture's order).
Bytes bgra_to_texels(const Bytes &bgra, TextureSize size)
{
    const auto stride = widen(size.width) * 4;
    Bytes texels;
    texels.reserve(widen(size.width) * size.height * 2);

    for (std::size_t y = size.height; y-- > 0;) {
        for (std::size_t x = 0; x < size.width; x++) {
            const auto i = y * stride + x * 4;
            const auto b = to_4bit(bgra.at(i));
            const auto g = to_4bit(bgra.at(i + 1));
            const auto r = to_4bit(bgra.at(i + 2));
            const auto a = to_4bit(bgra.at(i + 3));
            const auto texel = a << 12 | r << 8 | g << 4 | b;

            texels.push_back(hy::narrow<std::uint8_t>(texel & 0xff));
            texels.push_back(hy::narrow<std::uint8_t>(texel >> 8));
        }
    }
    return texels;
}

// ---- The overlay archive ----

struct Object {
    std::string name;
    const Bytes *dir_template; // HT.R2's directory entry for the template object
    Bytes record;
};

Bytes pack_archive(const Bytes &header_template, const std::vector<Object> &objects)
{
    const auto dir_size = kDirEntrySize * objects.size();
    const auto data_start = (kHeaderSize + dir_size + 15) & ~widen(15);

    Bytes out(header_template);
    write_u32(out, kHeaderDirOffset, hy::narrow<std::uint32_t>(kHeaderSize));
    write_u32(out, kHeaderDataStart, hy::narrow<std::uint32_t>(data_start));
    write_u32(out, kHeaderCount, hy::narrow<std::uint32_t>(objects.size()));
    out.resize(data_start);

    Bytes data;
    std::size_t entry_at = kHeaderSize;

    for (const auto &o : objects) {
        const auto body_size = read_u32(o.record, 0) & 0xffffff;
        const auto nrefs = read_u32(o.record, 4 + widen(body_size));
        const auto record_at = data_start + data.size();

        Bytes name;
        append_name(name, o.name);

        const auto entry = std::span(out).subspan(entry_at, kDirEntrySize);
        std::ranges::copy(*o.dir_template, entry.begin());
        write_u32(entry, kEntryOffset, hy::narrow<std::uint32_t>(record_at));
        write_u32(entry, kEntryBodySize, body_size);
        write_u32(entry, kEntryRefCount, nrefs);
        std::ranges::copy(name, entry.subspan(kEntryName, kNameSize).begin());

        data.insert(data.end(), o.record.begin(), o.record.end());
        data.resize((data.size() + 3) & ~widen(3)); // records are 4-byte aligned

        entry_at += kDirEntrySize;
    }

    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::optional<Bytes> build_overlay(const std::string &ht_r2_path)
{
    auto archive = R2Archive::open(ht_r2_path);
    if (!archive) {
        hy_log("markers: %s is not readable as an R2 archive", ht_r2_path.c_str());
        return std::nullopt;
    }

    const auto tex = archive->read(kTemplateTexture);
    const auto mesh = archive->read(kTemplateMesh);
    const auto size = tex ? marker_texture_size(tex->body) : std::nullopt;
    const auto ref = mesh ? texture_ref(*mesh) : std::nullopt;
    if (!size || !ref) {
        hy_log("markers: HT.R2's marker 8 isn't the expected mesh and texture; no markers 9-16");
        return std::nullopt;
    }

    const ComScope com;
    ComPtr<IWICImagingFactory> wic;
    const bool have_wic =
        com.usable() &&
        hy::succeeded(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
    if (!have_wic) {
        hy_log("markers: WIC is unavailable; no markers 9-16");
        return std::nullopt;
    }

    std::vector<Object> objects;
    for (int n = kFirstMarker; n <= kLastMarker; n++) {
        const auto mesh_name = marker_name("GHWPLAYID", n);
        const auto tex_name = marker_name("THWPLAYID", n);
        if (archive->dir_entry(mesh_name) || archive->dir_entry(tex_name))
            continue; // HT.R2 has it already

        auto png = load_resource(MARKERS_RESOURCE_BASE + n);
        const auto bgra = png ? decode_bgra(*wic.Get(), std::move(*png), *size) : std::nullopt;
        if (!bgra) {
            hy_log("markers: marker%02d.png is missing or not %ux%u", n, size->width, size->height);
            continue;
        }

        // The texture: the template with this marker's texels.
        Record t = *tex;
        const auto texels = bgra_to_texels(*bgra, *size);
        std::ranges::copy(texels, std::span(t.body).subspan(kTexelOffset, texels.size()).begin());

        // The mesh: the template, pointing at that texture.
        Record m = *mesh;
        m.refs.at(*ref).name = tex_name;

        objects.push_back({tex_name, archive->dir_entry(kTemplateTexture), t.serialize(tex_name)});
        objects.push_back({mesh_name, archive->dir_entry(kTemplateMesh), m.serialize(mesh_name)});
    }

    if (objects.empty())
        return std::nullopt;

    return pack_archive(archive->header(), objects);
}

} // namespace

extern "C" uint8_t *markers_build_overlay(const char *ht_r2_path, uint32_t *size)
{
    if (!ht_r2_path || !size)
        return nullptr;

    *size = 0;
    try {
        const auto overlay = build_overlay(ht_r2_path);
        if (!overlay)
            return nullptr;

        // The C caller owns the buffer and frees it with free().
        auto *out = static_cast<uint8_t *>(std::malloc(overlay->size()));
        if (!out)
            return nullptr;

        std::memcpy(out, overlay->data(), overlay->size());
        *size = hy::narrow<uint32_t>(overlay->size());
        return out;
    } catch (const std::exception &e) {
        hy_log("markers: %s; no markers 9-16", e.what());
        return nullptr;
    }
}
