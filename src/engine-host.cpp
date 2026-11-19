#include "key-protocol.hpp"
#include "native-settings.hpp"

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Private interfaces of Microsoft Chinese Keyboard 1.0.0.17694 (x64).
constexpr GUID PinyinSource = {0xae2753ca, 0x51a8, 0x42d7, {0x81, 0xa0, 0x68, 0x35, 0x5e, 0xdb, 0x3f, 0x6f}};
constexpr GUID CloudSource = {0x9a3b7c5d, 0x1e4f, 0x6a8b, {0x2c, 0x9d, 0x0e, 0x5f, 0x7a, 0x1b, 0x3c, 0x4d}};
constexpr GUID AdvancedSource = {0x985e6333, 0xff49, 0x4c1b, {0x8d, 0x88, 0xee, 0xd1, 0x32, 0x7c, 0x77, 0x80}};
constexpr GUID FullPinyin = {0x6035ea3f, 0x3752, 0x4739, {0xb3, 0x13, 0x75, 0xb9, 0xff, 0xc4, 0xb2, 0x45}};
constexpr GUID QueryClass = {0xb35e5cd2, 0xe69d, 0x476b, {0xbf, 0x0f, 0xf5, 0x44, 0xa6, 0x75, 0xc6, 0x89}};
constexpr GUID QueryInterface = {0xc1b56759, 0xfcae, 0x4e27, {0x93, 0x85, 0xbe, 0x45, 0xbb, 0xf0, 0xc1, 0x62}};
constexpr GUID BagClass = {0xc36fb162, 0x7dd6, 0x4bea, {0x85, 0x04, 0xa7, 0xaa, 0x53, 0x40, 0xe0, 0x2d}};
constexpr GUID BagInterface = {0x7b07de75, 0xe88d, 0x4407, {0x87, 0x67, 0x32, 0x2f, 0x00, 0x51, 0x77, 0x29}};
constexpr GUID PinyinInput = {0x6a71c1c2, 0xad57, 0x6f4e, {0xa0, 0x69, 0xa9, 0x2d, 0x55, 0x3a, 0x75, 0x73}};
constexpr GUID AdvancedType = {0x9ea6d031, 0x12bf, 0x420a, {0x92, 0x16, 0xc3, 0x5d, 0x54, 0x0c, 0x7a, 0xb8}};
constexpr GUID CandidateInterface = {0x835a2de2, 0x8c98, 0x404b, {0xbe, 0xf3, 0x27, 0x8c, 0x27, 0xb3, 0xdd, 0xee}};
constexpr size_t ProbePageSize = 9;
constexpr size_t MaxInput = 512;
enum class QuerySource { Pinyin, Advanced, Cloud };

struct DataSourceInit {
    DWORD fields = 1;
    LANGID language = 0x0804;
    WORD reserved0 = 0;
    const DWORD *options = nullptr;
    DWORD optionCount = 0;
    BYTE reserved1[16] = {};
    GUID inputMode = FullPinyin;
    IUnknown *propertyBag = nullptr;
};

struct CandidateProperties {
    DWORD fields = 0x83;
    BSTR reading = nullptr;
    BSTR text = nullptr;
    BYTE reserved[24] = {};
    const BYTE *details = nullptr;
    DWORD detailsSize = 0;
    DWORD trailing[3] = {};
};

static_assert(offsetof(DataSourceInit, inputMode) == 0x24);
static_assert(offsetof(DataSourceInit, propertyBag) == 0x38);
static_assert(offsetof(CandidateProperties, text) == 0x10);
static_assert(offsetof(CandidateProperties, details) == 0x30);

struct Release {
    void operator()(IUnknown *object) const { object->Release(); }
};

using ComObject = std::unique_ptr<IUnknown, Release>;

ComObject create(REFCLSID clsid, REFIID iid) {
    IUnknown *object = nullptr;
    CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, reinterpret_cast<void **>(&object));
    return ComObject(object);
}

void **table(IUnknown *object) {
    return *reinterpret_cast<void ***>(object);
}

bool hasTable(IUnknown *object, HMODULE module, ULONG_PTR rva) {
    return object && module && reinterpret_cast<ULONG_PTR>(table(object)) ==
        reinterpret_cast<ULONG_PTR>(module) + rva;
}

template<typename F>
F method(IUnknown *object, size_t index) {
    return reinterpret_cast<F>(table(object)[index]);
}

template<typename F>
F procedure(HMODULE module, const char *name) {
    return reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(module, name)));
}

std::string utf8(const std::wstring &text) {
    if (text.empty()) return {};
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), size, nullptr, nullptr);
    return result;
}

struct Candidate {
    std::wstring reading;
    std::wstring text;
    std::wstring consumedPinyin;
    ComObject sourceCandidate;
};

std::wstring consumedPinyin(const CandidateProperties &fields) {
    // ChsPinyinDS candidate data starts with 0xfe02 and stores UTF-16 string offsets at bytes 8 and 10.
    if (!fields.details || fields.detailsSize < 28) return {};
    DWORD format = 0;
    WORD start = 0;
    WORD end = 0;
    std::memcpy(&format, fields.details, sizeof(format));
    std::memcpy(&start, fields.details + 8, sizeof(start));
    std::memcpy(&end, fields.details + 10, sizeof(end));
    if (format != 0xfe02 || start < 28 || end <= start || end > fields.detailsSize ||
        (start % sizeof(wchar_t)) || (end % sizeof(wchar_t))) return {};
    std::wstring result((end - start) / sizeof(wchar_t), L'\0');
    std::memcpy(result.data(), fields.details + start, end - start);
    if (result.back() != L'\0') return {};
    result.pop_back();
    return result;
}

class PinyinEngine {
public:
    bool initialize(bool learning) {
        SetDllDirectoryW(L"C:\\Program Files\\BingKeyboard\\Server");
        HMODULE serverModule = LoadLibraryW(L"BingIMEMtfServer.dll");
        if (!serverModule) return false;
        const auto privateMode = procedure<BOOL (WINAPI *)(BOOL)>(serverModule, "MTF_SetPrivateMode");
        const auto init = procedure<DWORD (WINAPI *)()>(serverModule, "MTF_Init");
        const auto getServer = procedure<HRESULT (WINAPI *)(IUnknown **)>(serverModule, "MTF_GetServer");
        if (!privateMode || !init || !getServer || !privateMode(TRUE) || !init()) return false;
        IUnknown *server = nullptr;
        if (FAILED(getServer(&server)) || !server) return false;
        server_.reset(server);

        mtfModule_ = LoadLibraryW(L"BingIMEMtf.dll");
        HMODULE pinyinModule = LoadLibraryW(L"MAI_ChsPinyinDS.dll");
        source_ = create(PinyinSource, IID_IUnknown);
        if (!hasTable(source_.get(), pinyinModule, 0x185f10) || !mtfModule_) return false;
        const DataSourceInit settings;
        using Initialize = HRESULT (WINAPI *)(IUnknown *, const DataSourceInit *);
        if (FAILED(method<Initialize>(source_.get(), 3)(source_.get(), &settings))) return false;
        if (learning) {
            wchar_t appData[MAX_PATH];
            const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
            if (!length || length >= MAX_PATH) return false;
            std::error_code error;
            std::filesystem::create_directories(
                std::filesystem::path(appData) / L"Microsoft" / L"BingKeyboard" / L"CHS", error);
            if (error) return false;

            // The Windows frontend opens the user's lexicon and history after initializing the data source.
            using PrepareLearning = void (WINAPI *)(IUnknown *);
            const auto prepare = reinterpret_cast<PrepareLearning>(reinterpret_cast<ULONG_PTR>(pinyinModule) + 0x18eb0);
            prepare(source_.get());
        }
        return true;
    }

    bool candidates(const std::wstring &input, std::vector<Candidate> &output,
                    QuerySource querySource = QuerySource::Pinyin, bool keepSourceCandidates = false,
                    DWORD advancedType = 1) const {
        output.clear();
        if (input.empty()) return true;
        if (input.size() > MaxInput) return false;

        auto query = create(QueryClass, QueryInterface);
        auto bag = create(BagClass, BagInterface);
        if (!hasTable(query.get(), mtfModule_, 0x2c3db8) || !hasTable(bag.get(), mtfModule_, 0x2c37a8))
            return false;

        // The first field is the number of UTF-16 code units in the following pinyin string.
        const DWORD length = static_cast<DWORD>(input.size());
        const bool cloudProbe = querySource == QuerySource::Cloud;
        const bool advancedProbe = querySource == QuerySource::Advanced;
        std::vector<BYTE> packet(sizeof(length) + length * sizeof(wchar_t) + (cloudProbe ? 4 * sizeof(DWORD) : 0));
        std::memcpy(packet.data(), &length, sizeof(length));
        std::memcpy(packet.data() + sizeof(length), input.data(), length * sizeof(wchar_t));

        using QueryParams = HRESULT (WINAPI *)(IUnknown *, DWORD *);
        using SetProperty = HRESULT (WINAPI *)(IUnknown *, REFGUID, const void *, DWORD);
        using AttachBag = HRESULT (WINAPI *)(IUnknown *, IUnknown *);
        using SetBuffer = HRESULT (WINAPI *)(IUnknown *, const void *, DWORD);
        using RunQuery = HRESULT (WINAPI *)(IUnknown *, IUnknown *, IUnknown **);
        DWORD params[24] = {1, 2}; // Set the query type to Pinyin conversion.
        HRESULT hr = method<QueryParams>(query.get(), 4)(query.get(), params);
        if (SUCCEEDED(hr) && querySource != QuerySource::Pinyin)
            hr = method<SetBuffer>(query.get(), 9)(query.get(), packet.data(), packet.size());
        if (SUCCEEDED(hr) && !cloudProbe) {
            hr = method<SetProperty>(bag.get(), 3)(bag.get(), PinyinInput, packet.data(), packet.size());
            if (SUCCEEDED(hr) && advancedProbe)
                hr = method<SetProperty>(bag.get(), 3)(bag.get(), AdvancedType, &advancedType, sizeof(advancedType));
            if (SUCCEEDED(hr)) hr = method<AttachBag>(query.get(), 7)(query.get(), bag.get());
        }
        if (FAILED(hr)) return false;

        IUnknown *source = source_.get();
        ComObject otherSource;
        if (cloudProbe || advancedProbe) {
            const auto &clsid = cloudProbe ? CloudSource : AdvancedSource;
            HMODULE module = LoadLibraryW(cloudProbe ? L"MAI_ChsProxyDS.dll" : L"MAI_ChsAdvancedDS.dll");
            otherSource = create(clsid, IID_IUnknown);
            source = otherSource.get();
            if (!hasTable(source, module, cloudProbe ? 0xfa210 : 0x142d10)) {
                std::fputs("Could not load Microsoft optional data source\n", stderr);
                return false;
            }
            const DataSourceInit settings;
            using Initialize = HRESULT (WINAPI *)(IUnknown *, const DataSourceInit *);
            hr = method<Initialize>(source, 3)(source, &settings);
            std::fprintf(stderr, "Microsoft %s initialize = 0x%08lx\n", cloudProbe ? "cloud" : "advanced",
                         static_cast<unsigned long>(hr));
            if (FAILED(hr)) return false;
        }
        IUnknown *suggestions = nullptr;
        hr = method<RunQuery>(source, 10)(source, query.get(), &suggestions);
        ComObject result(suggestions);
        if (cloudProbe || advancedProbe)
            std::fprintf(stderr, "Microsoft %s query = 0x%08lx, result = %p\n",
                         cloudProbe ? "cloud" : "advanced", static_cast<unsigned long>(hr),
                         static_cast<void *>(suggestions));
        if (FAILED(hr)) {
            std::fprintf(stderr, "Microsoft Pinyin query failed (0x%08lx)\n", static_cast<unsigned long>(hr));
            return false;
        }
        if (!result) return true;
        if (!hasTable(result.get(), mtfModule_, 0x2d7f78)) return false;

        using Count = HRESULT (WINAPI *)(IUnknown *, DWORD *);
        using Item = HRESULT (WINAPI *)(IUnknown *, DWORD, IUnknown **);
        using Read = HRESULT (WINAPI *)(IUnknown *, CandidateProperties *);
        DWORD count = 0;
        if (FAILED(method<Count>(result.get(), 3)(result.get(), &count))) return false;
        output.reserve(count);
        for (DWORD index = 0; index < count; ++index) {
            IUnknown *item = nullptr;
            if (FAILED(method<Item>(result.get(), 4)(result.get(), index, &item)) || !item) return false;
            ComObject entry(item);
            IUnknown *typed = nullptr;
            entry->QueryInterface(CandidateInterface, reinterpret_cast<void **>(&typed));
            ComObject candidate(typed);
            if (!hasTable(candidate.get(), mtfModule_, 0x2d7d48)) return false;

            CandidateProperties fields;
            if (FAILED(method<Read>(candidate.get(), 5)(candidate.get(), &fields))) return false;
            if (fields.text && SysStringLen(fields.text) > 0) {
                output.push_back({fields.reading ? std::wstring(fields.reading, SysStringLen(fields.reading)) : L"",
                                  std::wstring(fields.text, SysStringLen(fields.text)),
                                  consumedPinyin(fields), keepSourceCandidates ? std::move(candidate) : ComObject()});
            }
        }
        return true;
    }

    HRESULT submitSelection(const Candidate &candidate) const {
        auto *composite = candidate.sourceCandidate.get();
        if (!composite) return E_INVALIDARG;
        using Count = HRESULT (WINAPI *)(IUnknown *, DWORD *);
        using Item = HRESULT (WINAPI *)(IUnknown *, DWORD, IUnknown **);
        using SetCandidateBag = HRESULT (WINAPI *)(IUnknown *, IUnknown *);
        using Read = HRESULT (WINAPI *)(IUnknown *, CandidateProperties *);
        using Submit = HRESULT (WINAPI *)(IUnknown *, IUnknown *, const BYTE *, DWORD);
        DWORD count = 0;
        HRESULT hr = method<Count>(composite, 7)(composite, &count);
        if (FAILED(hr)) return hr;
        const BYTE feedback[] = {1, 1};
        for (DWORD index = 0; index < count; ++index) {
            IUnknown *item = nullptr;
            hr = method<Item>(composite, 8)(composite, index, &item);
            ComObject scalar(item);
            if (FAILED(hr) || !scalar) return FAILED(hr) ? hr : E_FAIL;
            // Submit the text and reading as selection feedback without the query's display metadata.
            hr = method<SetCandidateBag>(scalar.get(), 8)(scalar.get(), nullptr);
            if (FAILED(hr)) return hr;
            CandidateProperties fields;
            fields.fields = 1;
            hr = method<Read>(scalar.get(), 5)(scalar.get(), &fields);
            if (FAILED(hr) || !fields.reading) return FAILED(hr) ? hr : E_FAIL;
            std::wstring reading(fields.reading, SysStringLen(fields.reading));
            std::replace(reading.begin(), reading.end(), L'\\', L' ');
            fields.reading = SysAllocStringLen(reading.data(), reading.size());
            if (!fields.reading) return E_OUTOFMEMORY;
            hr = method<Read>(scalar.get(), 6)(scalar.get(), &fields);
            if (FAILED(hr)) {
                SysFreeString(fields.reading);
                return hr;
            }
            hr = method<Submit>(source_.get(), 7)(source_.get(), scalar.get(), feedback, sizeof(feedback));
            if (FAILED(hr)) return hr;
        }
        return count ? S_OK : S_FALSE;
    }

private:
    HMODULE mtfModule_ = nullptr;
    ComObject server_;
    ComObject source_;
};

std::string hex(const std::wstring &text) {
    const auto bytes = utf8(text);
    if (bytes.empty()) return "-";
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 0x0f];
    }
    return result;
}

class InputSession {
public:
    InputSession(const PinyinEngine &engine, bool learning)
        : engine_(engine), learning_(learning) {}

    void run() {
        for (std::string line; std::getline(std::cin, line);) {
            std::istringstream fields(line);
            char command;
            unsigned sequence;
            if (!(fields >> command >> sequence)) continue;
            bool handled = false;
            unsigned argument;
            unsigned release;
            switch (command) {
            case 'H': {
                unsigned options = ime::ChinesePunctuation;
                unsigned size = ime::DefaultPageSize;
                fields >> size >> options;
                clear();
                pageSize_ = std::clamp(size, 3u, 9u);
                chinesePunctuation_ = (options & ime::ChinesePunctuation) != 0;
                fullWidth_ = (options & ime::FullWidth) != 0;
                english_ = false;
                showLanguage();
                handled = true;
                break;
            }
            case 'R':
                clear();
                handled = true;
                break;
            case 'K':
                if (fields >> argument >> release && release == 0) handled = key(argument);
                break;
            case 'E': {
                ime::NativeSettings settings;
                if (!(fields >> settings.autoCorrection >> settings.fuzzyEnabled >> settings.fuzzyPairs >> settings.selfLearning)) break;
                const LONG error = ime::writeNativeSettings(settings);
                std::cout << "W " << sequence << ' ' << error << std::endl;
                continue;
            }
            case 'S':
                if (fields >> argument) handled = select(argument);
                break;
            }
            std::cout << "A " << sequence << ' ' << handled << std::endl;
        }
    }

private:
    void clear() {
        input_.clear();
        selectedText_.clear();
        segments_.clear();
        candidates_.clear();
        selected_ = 0;
        page_ = 0;
    }

    void show() const {
        std::cout << "U " << page_ << ' ' << selected_ << ' ' << hex(selectedText_ + input_);
        const size_t end = std::min(page_ + pageSize_, candidates_.size());
        for (size_t index = page_; index < end; ++index) std::cout << ' ' << hex(candidates_[index].text);
        std::cout << '\n';
    }

    void showLanguage() const {
        std::cout << "M " << english_ << '\n';
    }

    bool refresh() {
        page_ = selected_ = 0;
        if (!engine_.candidates(input_, candidates_, QuerySource::Pinyin, learning_)) {
            candidates_.clear();
            return false;
        }
        show();
        return true;
    }

    bool commit(const std::wstring &text) {
        std::cout << "C " << hex(text) << '\n';
        clear();
        show();
        return true;
    }

    bool select(size_t index, std::wstring_view suffix = {}) {
        if (index >= candidates_.size()) return false;
        auto &candidate = candidates_[index];
        size_t consumed = 0;
        for (wchar_t letter : candidate.consumedPinyin) {
            if (letter == L'\'') continue;
            while (consumed < input_.size() && input_[consumed] == L'\'') ++consumed;
            if (consumed == input_.size() || input_[consumed] != letter) {
                consumed = 0;
                break;
            }
            ++consumed;
        }
        if (consumed) {
            while (consumed < input_.size() && input_[consumed] == L'\'') ++consumed;
        }
        if (consumed && consumed < input_.size()) {
            selectedText_ += candidate.text;
            segments_.push_back({input_.substr(0, consumed), std::move(candidate)});
            input_.erase(0, consumed);
            if (!refresh() || suffix.empty()) return true;
            if (!candidates_.empty()) return select(0, suffix);
            return commit(selectedText_ + input_ + std::wstring(suffix));
        }
        const auto text = selectedText_ + candidate.text + std::wstring(suffix);
        if (learning_) {
            for (const auto &segment : segments_) learn(segment.candidate);
            learn(candidate);
        }
        return commit(text);
    }

    void learn(const Candidate &candidate) const {
        const HRESULT hr = engine_.submitSelection(candidate);
        if (FAILED(hr))
            std::fprintf(stderr, "Microsoft Pinyin selection feedback failed (0x%08lx)\n",
                         static_cast<unsigned long>(hr));
    }

    bool navigate(unsigned key) {
        if (candidates_.empty()) return false;
        if (key == VK_PRIOR) selected_ = page_ >= pageSize_ ? page_ - pageSize_ : 0;
        if (key == VK_NEXT) selected_ = std::min(candidates_.size() - 1, page_ + pageSize_);
        if ((key == VK_LEFT || key == VK_UP) && selected_ > 0) --selected_;
        if (key == VK_RIGHT || key == VK_DOWN) selected_ = std::min(candidates_.size() - 1, selected_ + 1);
        page_ = selected_ / pageSize_ * pageSize_;
        show();
        return true;
    }

    wchar_t punctuation(wchar_t character) {
        if (chinesePunctuation_) {
            switch (character) {
            case ',': return L'，';
            case '.': return L'。';
            case '?': return L'？';
            case '!': return L'！';
            case ':': return L'：';
            case ';': return L'；';
            case '(': return L'（';
            case ')': return L'）';
            case '[': return L'【';
            case ']': return L'】';
            case '<': return L'《';
            case '>': return L'》';
            case '\\': return L'、';
            case '"':
                doubleQuoteOpen_ = !doubleQuoteOpen_;
                return doubleQuoteOpen_ ? L'“' : L'”';
            case '\'':
                singleQuoteOpen_ = !singleQuoteOpen_;
                return singleQuoteOpen_ ? L'‘' : L'’';
            }
        }
        if (fullWidth_) return character == ' ' ? L'　' : character + 0xfee0;
        return character;
    }

    bool key(unsigned key) {
        if (key == ime::ToggleLanguage) {
            if (!input_.empty() || !selectedText_.empty()) commit(selectedText_ + input_);
            english_ = !english_;
            showLanguage();
            return true;
        }
        if (key == ime::TogglePunctuation) {
            chinesePunctuation_ = !chinesePunctuation_;
            return true;
        }
        if (key == ime::ToggleWidth) {
            fullWidth_ = !fullWidth_;
            return true;
        }
        if (english_) {
            if (!fullWidth_) return false;
            if (key >= ime::PrintableAscii + '!' && key <= ime::PrintableAscii + '~')
                return commit(std::wstring(1, static_cast<wchar_t>(key - ime::PrintableAscii + 0xfee0)));
            if (key == VK_SPACE) return commit(L"　");
            if (key >= '0' && key <= '9') return commit(std::wstring(1, static_cast<wchar_t>(key + 0xfee0)));
            return false;
        }

        const bool separator = key == ime::PrintableAscii + '\'' && !input_.empty();
        if ((key >= 'A' && key <= 'Z') || separator) {
            if (input_.size() >= MaxInput) return true;
            const wchar_t letter = separator ? L'\'' : static_cast<wchar_t>(key - 'A' + L'a');
            input_.push_back(letter);
            if (refresh()) return true;
            input_.pop_back();
            refresh();
            return false;
        }
        if (key >= ime::PrintableAscii + '!' && key <= ime::PrintableAscii + '~') {
            const auto character = static_cast<wchar_t>(key - ime::PrintableAscii);
            const wchar_t mapped = punctuation(character);
            if (input_.empty() && selectedText_.empty() && mapped == character) return false;
            const std::wstring suffix(1, mapped);
            if (!candidates_.empty()) return select(selected_, suffix);
            return commit(selectedText_ + input_ + suffix);
        }
        if (input_.empty() && selectedText_.empty()) {
            if (fullWidth_ && (key == VK_SPACE || (key >= '0' && key <= '9')))
                return commit(std::wstring(1, punctuation(static_cast<wchar_t>(key))));
            return false;
        }
        switch (key) {
        case VK_ESCAPE:
            clear();
            show();
            return true;
        case VK_BACK:
            if (!input_.empty()) {
                input_.pop_back();
            } else if (!segments_.empty()) {
                input_ = std::move(segments_.back().pinyin);
                selectedText_.resize(selectedText_.size() - segments_.back().candidate.text.size());
                segments_.pop_back();
            }
            refresh();
            return true;
        case VK_RETURN:
            return commit(selectedText_ + input_);
        case VK_SPACE:
            if (!candidates_.empty()) return select(selected_);
            return commit(selectedText_ + input_);
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            return navigate(key);
        default:
            if (key >= '1' && key < '1' + pageSize_) return select(page_ + key - '1');
            return false;
        }
    }

    const PinyinEngine &engine_;
    const bool learning_;
    struct Segment {
        std::wstring pinyin;
        Candidate candidate;
    };
    std::wstring input_;
    std::wstring selectedText_;
    std::vector<Segment> segments_;
    std::vector<Candidate> candidates_;
    size_t page_ = 0;
    size_t pageSize_ = ime::DefaultPageSize;
    size_t selected_ = 0;
    bool chinesePunctuation_ = true;
    bool fullWidth_ = false;
    bool english_ = false;
    bool doubleQuoteOpen_ = false;
    bool singleQuoteOpen_ = false;
};

int run(int argc, wchar_t **argv) {
    const bool learningProbe = argc > 1 && std::wstring_view(argv[1]) == L"--learning-probe";
    const bool readLearning = argc > 1 && std::wstring_view(argv[1]) == L"--learning-readback";
    const bool interactiveLearning = argc == 2 && std::wstring_view(argv[1]) == L"--interactive-with-learning";
    const auto native = ime::readNativeSettings();
    const bool learn = interactiveLearning && native.selfLearning != 0;
    PinyinEngine engine;
    if (!engine.initialize(learningProbe || readLearning || learn)) {
        std::fputs("Microsoft Pinyin initialization failed. Expected x64 engine version 1.0.0.17694.\n", stderr);
        return 1;
    }
    if (argc > 1 && !interactiveLearning) {
        if (learningProbe) {
            if (argc != 4) return 2;
            for (unsigned attempt = 0; attempt <= 12; ++attempt) {
                std::vector<Candidate> candidates;
                if (!engine.candidates(argv[2], candidates, QuerySource::Pinyin, true)) return 1;
                const auto position = std::find_if(candidates.begin(), candidates.end(), [argv](const Candidate &candidate) {
                    return candidate.text == argv[3];
                });
                if (position == candidates.end()) return 1;
                std::cout << attempt << '\t' << position - candidates.begin() + 1 << '\t'
                          << utf8(position->text) << std::endl;
                if (attempt == 12) break;
                const HRESULT hr = engine.submitSelection(*position);
                std::fprintf(stderr, "Microsoft selection feedback = 0x%08lx\n", static_cast<unsigned long>(hr));
                if (FAILED(hr)) return 1;
            }
            return 0;
        }
        const bool cloudProbe = std::wstring_view(argv[1]) == L"--cloud-probe";
        const bool advancedProbe = std::wstring_view(argv[1]) == L"--advanced-probe";
        if (cloudProbe && argc != 3) return 2;
        const auto source = cloudProbe ? QuerySource::Cloud : advancedProbe ? QuerySource::Advanced : QuerySource::Pinyin;
        for (int index = cloudProbe || advancedProbe || readLearning ? 2 : 1; index < argc; ++index) {
            std::vector<Candidate> candidates;
            if (advancedProbe) {
                for (DWORD type = 1; type <= 3; ++type) {
                    const bool found = engine.candidates(argv[index], candidates, source, false, type);
                    std::cout << "Advanced " << type << '\t' << utf8(argv[index])
                              << '\t' << found << '\t' << candidates.size() << '\n';
                    for (const auto &candidate : candidates) std::cout << utf8(candidate.text) << '\n';
                }
                continue;
            }
            if (!engine.candidates(argv[index], candidates, source)) return 1;
            std::cout << utf8(argv[index]) << '\t' << candidates.size() << '\n';
            for (size_t item = 0; item < std::min(candidates.size(), ProbePageSize); ++item) {
                std::cout << item + 1 << '\t' << utf8(candidates[item].text)
                          << '\t' << utf8(candidates[item].reading)
                          << '\t' << utf8(candidates[item].consumedPinyin) << '\n';
            }
        }
        return 0;
    }
    std::cout << "N " << native.autoCorrection << ' ' << native.fuzzyEnabled << ' ' << native.fuzzyPairs << ' '
              << native.selfLearning << '\n';
    std::cout << "READY" << std::endl;
    InputSession(engine, learn).run();
    return 0;
}

}

int wmain(int argc, wchar_t **argv) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    const int result = run(argc, argv);
    CoUninitialize();
    return result;
}
