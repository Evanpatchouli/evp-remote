#include "host_input.h"

#include <QHash>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace evp {
namespace {

const auto desktopUnavailable = QStringLiteral("Windows 交互桌面不可用。请解锁本机并返回普通桌面。");
const auto inputRejected = QStringLiteral("Windows 拒绝了键鼠输入。请返回普通桌面后重新创建邀请。");

bool hasObjectName(HANDLE object, const wchar_t* expected) {
    wchar_t name[64]{};
    DWORD required = 0;
    return object && GetUserObjectInformationW(object, UOI_NAME, name, sizeof(name), &required) &&
           CompareStringOrdinal(name, -1, expected, -1, TRUE) == CSTR_EQUAL;
}

bool isDefaultDesktop() {
    if (!hasObjectName(GetProcessWindowStation(), L"WinSta0") ||
        !hasObjectName(GetThreadDesktop(GetCurrentThreadId()), L"Default"))
        return false;
    const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop)
        return false;
    const bool available = hasObjectName(desktop, L"Default");
    CloseDesktop(desktop);
    return available;
}

QString checkWindowsEnvironment() {
    if (GetSystemMetrics(SM_CMONITORS) != 1)
        return QStringLiteral("远程键鼠需要本机仅启用一个显示器。");
    return isDefaultDesktop() ? QString{} : desktopUnavailable;
}

bool sendWindowsInput(const INPUT& input) {
    auto pending = input;
    return isDefaultDesktop() && SendInput(1, &pending, sizeof(pending)) == 1;
}

INPUT mouse(DWORD flags, LONG x = 0, LONG y = 0, DWORD data = 0) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.mouseData = data;
    input.mi.dwFlags = flags;
    return input;
}

INPUT key(int scan, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = static_cast<WORD>(scan & 0xff);
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (scan > 0xff)
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down)
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    return input;
}

constexpr DWORD buttonDown[] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_RIGHTDOWN};
constexpr DWORD buttonUp[] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_RIGHTUP};

const QHash<QString, int>& scanCodes() {
    // DOM code allowlist and Windows set-1 scan codes from desktop/input.mjs.
    static const QHash<QString, int> codes{
        {"Escape", 0x01},
        {"Digit1", 0x02}, {"Digit2", 0x03}, {"Digit3", 0x04}, {"Digit4", 0x05}, {"Digit5", 0x06},
        {"Digit6", 0x07}, {"Digit7", 0x08}, {"Digit8", 0x09}, {"Digit9", 0x0a}, {"Digit0", 0x0b},
        {"Minus", 0x0c}, {"Equal", 0x0d}, {"Backspace", 0x0e}, {"Tab", 0x0f},
        {"KeyQ", 0x10}, {"KeyW", 0x11}, {"KeyE", 0x12}, {"KeyR", 0x13}, {"KeyT", 0x14},
        {"KeyY", 0x15}, {"KeyU", 0x16}, {"KeyI", 0x17}, {"KeyO", 0x18}, {"KeyP", 0x19},
        {"BracketLeft", 0x1a}, {"BracketRight", 0x1b}, {"Enter", 0x1c}, {"ControlLeft", 0x1d},
        {"KeyA", 0x1e}, {"KeyS", 0x1f}, {"KeyD", 0x20}, {"KeyF", 0x21}, {"KeyG", 0x22},
        {"KeyH", 0x23}, {"KeyJ", 0x24}, {"KeyK", 0x25}, {"KeyL", 0x26}, {"Semicolon", 0x27},
        {"Quote", 0x28}, {"Backquote", 0x29}, {"ShiftLeft", 0x2a}, {"Backslash", 0x2b},
        {"KeyZ", 0x2c}, {"KeyX", 0x2d}, {"KeyC", 0x2e}, {"KeyV", 0x2f}, {"KeyB", 0x30},
        {"KeyN", 0x31}, {"KeyM", 0x32}, {"Comma", 0x33}, {"Period", 0x34}, {"Slash", 0x35},
        {"ShiftRight", 0x36}, {"NumpadMultiply", 0x37}, {"AltLeft", 0x38}, {"Space", 0x39},
        {"CapsLock", 0x3a}, {"F1", 0x3b}, {"F2", 0x3c}, {"F3", 0x3d}, {"F4", 0x3e},
        {"F5", 0x3f}, {"F6", 0x40}, {"F7", 0x41}, {"F8", 0x42}, {"F9", 0x43}, {"F10", 0x44},
        {"F11", 0x57}, {"F12", 0x58}, {"NumLock", 0x45}, {"ScrollLock", 0x46},
        {"IntlBackslash", 0x56},
        {"Numpad7", 0x47}, {"Numpad8", 0x48}, {"Numpad9", 0x49}, {"NumpadSubtract", 0x4a},
        {"Numpad4", 0x4b}, {"Numpad5", 0x4c}, {"Numpad6", 0x4d}, {"NumpadAdd", 0x4e},
        {"Numpad1", 0x4f}, {"Numpad2", 0x50}, {"Numpad3", 0x51}, {"Numpad0", 0x52},
        {"NumpadDecimal", 0x53},
        {"NumpadEnter", 0xe01c}, {"ControlRight", 0xe01d}, {"NumpadDivide", 0xe035},
        {"PrintScreen", 0xe037}, {"AltRight", 0xe038}, {"Home", 0xe047}, {"ArrowUp", 0xe048},
        {"PageUp", 0xe049}, {"ArrowLeft", 0xe04b}, {"ArrowRight", 0xe04d}, {"End", 0xe04f},
        {"ArrowDown", 0xe050}, {"PageDown", 0xe051}, {"Insert", 0xe052}, {"Delete", 0xe053},
        {"MetaLeft", 0xe05b}, {"MetaRight", 0xe05c}, {"ContextMenu", 0xe05d},
    };
    return codes;
}

template <class Release>
void releaseReverse(std::vector<int>& held, Release release) {
    for (auto count = held.size(); count > 0; --count) {
        const auto item = held.begin() + (count - 1);
        if (release(*item))
            held.erase(item);
    }
}

} // namespace

HostInput::HostInput(Sender sender, EnvironmentCheck environmentCheck)
    : sender_(sender ? std::move(sender) : Sender{sendWindowsInput}),
      environmentCheck_(environmentCheck ? std::move(environmentCheck)
                                         : EnvironmentCheck{checkWindowsEnvironment}) {
    heldKeys_.reserve(scanCodes().size());
    heldButtons_.reserve(std::size(buttonDown));
}

HostInput::~HostInput() {
    disarm();
}

QString HostInput::checkEnvironment() const {
    return environmentCheck_();
}

QString HostInput::arm() {
    if (!disarm())
        return inputRejected;
    const auto error = checkEnvironment();
    armed_ = error.isEmpty();
    return error;
}

HostInput::Result HostInput::apply(const QJsonObject& event) {
    if (!armed_)
        return Result::Ignored;
    const auto type = event.value("type").toString();
    if (type == "release")
        return releaseAll() ? Result::Applied : fail();
    if (type == "ping")
        return checkEnvironment().isEmpty() ? Result::Applied : fail();

    INPUT input{};
    std::vector<int>* held = nullptr;
    int id = 0;
    bool down = false;
    if (type == "move") {
        const auto x = event.value("x").toDouble(-1.0);
        const auto y = event.value("y").toDouble(-1.0);
        if (!(x >= 0 && x <= 1 && y >= 0 && y <= 1))
            return Result::Ignored;
        input = mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
                      std::lround(x * 65535), std::lround(y * 65535));
    } else if (type == "wheel") {
        const auto delta = event.value("delta").toInt(32768);
        if (delta < -32768 || delta > 32767)
            return Result::Ignored;
        input = mouse(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(delta));
    } else if (type == "button" || type == "key") {
        const auto state = event.value("down");
        if (!state.isBool())
            return Result::Ignored;
        down = state.toBool();
        if (type == "button") {
            id = event.value("button").toInt(-1);
            if (id < 0 || id > 2)
                return Result::Ignored;
            input = mouse(down ? buttonDown[id] : buttonUp[id]);
            held = &heldButtons_;
        } else {
            id = scanCodes().value(event.value("code").toString(), 0);
            if (!id)
                return Result::Ignored;
            input = key(id, down);
            held = &heldKeys_;
        }
        const bool alreadyHeld = std::find(held->begin(), held->end(), id) != held->end();
        if ((!down && !alreadyHeld) || (down && alreadyHeld && type == "button"))
            return Result::Ignored;
    } else {
        return Result::Ignored;
    }

    if (!checkEnvironment().isEmpty() || !sender_(input))
        return fail();
    if (held) {
        const auto existing = std::find(held->begin(), held->end(), id);
        if (down && existing == held->end())
            held->push_back(id);
        else if (!down)
            held->erase(existing);
    }
    return Result::Applied;
}

bool HostInput::releaseAll() {
    releaseReverse(heldButtons_, [this](int id) { return sender_(mouse(buttonUp[id])); });
    releaseReverse(heldKeys_, [this](int id) { return sender_(key(id, false)); });
    return heldButtons_.empty() && heldKeys_.empty();
}

bool HostInput::disarm() {
    armed_ = false;
    return releaseAll();
}

HostInput::Result HostInput::fail() {
    disarm();
    return Result::Failed;
}

} // namespace evp
