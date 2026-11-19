#include "key-protocol.hpp"

#include <fcitx-config/configuration.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <sstream>
#include <string_view>

namespace {

FCITX_CONFIGURATION(
    InputMethodConfig,
    fcitx::Option<int, fcitx::IntConstrain> pageSize{
        this, "PageSize", "每页候选词个数", ime::DefaultPageSize, fcitx::IntConstrain(3, 9)};
    fcitx::Option<bool> shiftSwitch{this, "ShiftSwitch", "单按 Shift 切换中英文", true};
    fcitx::Option<bool> chinesePunctuation{this, "ChinesePunctuation", "默认使用中文标点", true};
    fcitx::Option<bool> fullWidth{this, "FullWidth", "默认使用全角字符", false};)

constexpr char configPath[] = "conf/microsoft-chinese-keyboard.conf";

int openEngineSocket() {
    const char *overridePath = std::getenv("MS_IME_SOCKET");
    const char *runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    const std::string socketPath = overridePath ? overridePath
        : runtimeDir ? std::string(runtimeDir) + "/microsoft-ime-engine.sock" : "";
    sockaddr_un address = {};
    if (socketPath.empty() || socketPath.size() >= sizeof(address.sun_path)) return -1;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0) return fd;
    close(fd);
    return -1;
}

std::string fromHex(std::string_view input) {
    if (input == "-" || input.size() % 2) return {};
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    for (size_t i = 0; i < input.size(); i += 2) {
        const auto high = digits.find(input[i]);
        const auto low = digits.find(input[i + 1]);
        if (high == digits.npos || low == digits.npos) return {};
        result.push_back(static_cast<char>(high * 16 + low));
    }
    return result;
}

unsigned windowsKey(const fcitx::Key &key, bool english) {
    if (key.check(FcitxKey_period, fcitx::KeyState::Ctrl)) return ime::TogglePunctuation;
    if (key.check(FcitxKey_space, fcitx::KeyState::Shift)) return ime::ToggleWidth;
    const auto shortcuts = fcitx::KeyStates(fcitx::KeyState::Ctrl) | fcitx::KeyState::Alt | fcitx::KeyState::Super;
    if (key.states().testAny(shortcuts)) return 0;
    const auto symbol = key.sym();
    if (english && ((symbol >= 'a' && symbol <= 'z') || (symbol >= 'A' && symbol <= 'Z'))) {
        const auto character = symbol >= 'a' && symbol <= 'z' && key.states().testAny(fcitx::KeyState::Shift)
            ? symbol - ('a' - 'A') : symbol;
        return ime::PrintableAscii + character;
    }
    if (symbol >= 'a' && symbol <= 'z') return 'A' + (symbol - 'a');
    if ((symbol >= 'A' && symbol <= 'Z') || (symbol >= '0' && symbol <= '9') || symbol == ' ') return symbol;
    switch (symbol) {
    case FcitxKey_BackSpace: return 0x08;
    case FcitxKey_Return: return 0x0d;
    case FcitxKey_Escape: return 0x1b;
    case FcitxKey_Page_Up: return 0x21;
    case FcitxKey_Page_Down: return 0x22;
    case FcitxKey_Left: return 0x25;
    case FcitxKey_Up: return 0x26;
    case FcitxKey_Right: return 0x27;
    case FcitxKey_Down: return 0x28;
    default: return symbol >= '!' && symbol <= '~' ? ime::PrintableAscii + symbol : 0;
    }
}

class RemoteCandidate : public fcitx::CandidateWord {
public:
    RemoteCandidate(std::string text, std::function<void(fcitx::InputContext *)> select)
        : CandidateWord(fcitx::Text(std::move(text))), select_(std::move(select)) {}
    void select(fcitx::InputContext *context) const override {
        auto action = select_;
        action(context);
    }
private:
    std::function<void(fcitx::InputContext *)> select_;
};

class MicrosoftEngine : public fcitx::InputMethodEngine {
public:
    explicit MicrosoftEngine(fcitx::Instance *instance) : instance_(instance) { reloadConfig(); }
    ~MicrosoftEngine() override { closeSocket(); }

    const fcitx::Configuration *getConfig() const override { return &config_; }
    void setConfig(const fcitx::RawConfig &config) override {
        config_.load(config, true);
        fcitx::safeSaveAsIni(config_, configPath);
        detach();
        closeSocket();
    }
    void reloadConfig() override {
        fcitx::readAsIni(config_, configPath);
        detach();
        closeSocket();
    }
    std::string subMode(const fcitx::InputMethodEntry &, fcitx::InputContext &) override {
        return english_ ? "英" : "中";
    }

    void activate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) override {
        auto *context = event.inputContext();
        if (fd_ < 0 && !context->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
            fd_ = openEngineSocket();
        }
    }

    void keyEvent(const fcitx::InputMethodEntry &, fcitx::KeyEvent &event) override {
        auto *context = event.inputContext();
        if (context->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
            detach();
            return;
        }
        unsigned key = windowsKey(event.key(), english_);
        const auto symbol = event.rawKey().sym();
        if (*config_.shiftSwitch && (symbol == FcitxKey_Shift_L || symbol == FcitxKey_Shift_R)) {
            if (!event.isRelease()) {
                shiftTap_ = event.rawKey().hasModifier() ? FcitxKey_None : symbol;
                return;
            }
            key = shiftTap_ == symbol ? ime::ToggleLanguage : 0;
            shiftTap_ = FcitxKey_None;
        } else if (!event.isRelease()) {
            shiftTap_ = FcitxKey_None;
        }
        if (!key) return;
        if (active_.get() != context) {
            detach();
            active_ = context->watch();
        }
        if (!connectEngine()) return;
        if (exchange('K', std::to_string(key) + (event.isRelease() && key != ime::ToggleLanguage ? " 1" : " 0"))) {
            event.filterAndAccept();
        }
    }

    void reset(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) override {
        if (active_.get() == event.inputContext()) detach();
    }

private:
    void detach() {
        auto *context = active_.get();
        active_.unwatch();
        if (connected_) exchange('R', {});
        if (context) {
            context->inputPanel().reset();
            context->updatePreedit();
            context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
    }

    bool connectEngine() {
        if (connected_) return true;
        if (fd_ < 0) fd_ = openEngineSocket();
        if (fd_ < 0) return false;
        const unsigned options = (*config_.chinesePunctuation ? ime::ChinesePunctuation : 0) |
                                 (*config_.fullWidth ? ime::FullWidth : 0);
        const std::string settings = std::to_string(*config_.pageSize) + ' ' + std::to_string(options);
        if (!exchange('H', settings)) {
            closeSocket();
            return false;
        }
        events_ = instance_->eventLoop().addIOEvent(
            fd_, fcitx::IOEventFlags(fcitx::IOEventFlag::In) | fcitx::IOEventFlag::Err | fcitx::IOEventFlag::Hup,
            [this](fcitx::EventSourceIO *source, int, fcitx::IOEventFlags) {
                if (receive()) return true;
                source->setEnabled(false);
                close(fd_);
                fd_ = -1;
                connected_ = false;
                buffer_.clear();
                detach();
                return false;
            });
        connected_ = true;
        return true;
    }

    void closeSocket() {
        events_.reset();
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        connected_ = false;
        buffer_.clear();
    }

    bool exchange(char operation, const std::string &argument) {
        using Clock = std::chrono::steady_clock;
        const unsigned request = ++sequence_;
        response_.reset();
        const std::string message = operation + (" " + std::to_string(request)) + " " + argument + '\n';
        if (send(fd_, message.data(), message.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(message.size())) {
            closeSocket();
            return false;
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds(160);
        pollfd event = {fd_, POLLIN, 0};
        while (Clock::now() < deadline) {
            const int delay = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
            if (poll(&event, 1, std::max(0, delay)) <= 0 || !receive()) break;
            if (response_.has_value()) return *response_;
        }
        closeSocket();
        return false;
    }

    bool receive() {
        char data[8192];
        const ssize_t size = recv(fd_, data, sizeof(data), 0);
        if (size < 0 && (errno == EAGAIN || errno == EINTR)) return true;
        if (size <= 0) return false;
        buffer_.append(data, size);
        size_t end;
        while ((end = buffer_.find('\n')) != std::string::npos) {
            std::string line = buffer_.substr(0, end);
            buffer_.erase(0, end + 1);
            handle(line);
        }
        return buffer_.size() <= 256 * 1024;
    }

    void handle(const std::string &line) {
        std::istringstream fields(line);
        char type;
        unsigned start, selection;
        if (!(fields >> type)) return;
        if (type == 'A' && fields >> start >> selection) {
            if (start == sequence_) response_ = selection == 1;
            return;
        }
        if (type == 'M' && fields >> start) {
            english_ = start == 1;
            return;
        }
        auto *context = active_.get();
        if (!context || !context->hasFocus() || context->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) return;
        std::string encoded;
        if (type == 'C' && fields >> encoded) {
            const std::string committed = fromHex(encoded);
            if (!committed.empty()) context->commitString(committed);
        }
        if (type == 'U' && fields >> start >> selection >> encoded) {
            auto candidates = std::make_unique<fcitx::CommonCandidateList>();
            std::vector<std::string> labels;
            const std::string preedit = fromHex(encoded);
            unsigned index = start;
            while (fields >> encoded && index - start < 64) {
                std::string text = fromHex(encoded);
                if (text.empty()) break;
                const unsigned candidateIndex = index++;
                const unsigned withinPage = candidateIndex - start;
                labels.push_back(withinPage < 9 ? std::to_string(withinPage + 1) + ". " : "");
                candidates->append<RemoteCandidate>(std::move(text), [this, candidateIndex](fcitx::InputContext *owner) {
                    if (active_.get() == owner && fd_ >= 0) exchange('S', std::to_string(candidateIndex));
                });
            }
            auto &panel = context->inputPanel();
            panel.reset();
            if (!preedit.empty()) {
                fcitx::Text text(preedit, fcitx::TextFormatFlag::Underline);
                text.setCursor(preedit.size());
                panel.setClientPreedit(std::move(text));
            }
            if (index > start) {
                candidates->setLabels(labels);
                candidates->setPageSize(static_cast<int>(index - start));
                if (selection >= start && selection < index) candidates->setGlobalCursorIndex(selection - start);
                panel.setCandidateList(std::move(candidates));
            }
            context->updatePreedit();
            context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
    }

    fcitx::Instance *instance_;
    InputMethodConfig config_;
    fcitx::KeySym shiftTap_ = FcitxKey_None;
    bool english_ = false;
    fcitx::TrackableObjectReference<fcitx::InputContext> active_;
    int fd_ = -1;
    bool connected_ = false;
    unsigned sequence_ = 0;
    std::string buffer_;
    std::optional<bool> response_;
    std::unique_ptr<fcitx::EventSourceIO> events_;
};

class MicrosoftEngineFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new MicrosoftEngine(manager->instance());
    }
};

}

FCITX_ADDON_FACTORY(MicrosoftEngineFactory)
