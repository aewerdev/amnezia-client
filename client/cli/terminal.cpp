#include "terminal.h"

#include <algorithm>
#include <cerrno>
#include <utility>

#include <QByteArray>
#include <QElapsedTimer>
#include <QtGlobal>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace amnezia::cli
{
struct TerminalSession::Impl
{
    bool active = false;

#ifdef Q_OS_WIN
    HANDLE input = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    DWORD inputMode = 0;
    DWORD outputMode = 0;
#elif defined(Q_OS_UNIX)
    termios inputMode {};
#endif
};

TerminalSession::TerminalSession() : d(std::make_unique<Impl>()) {}

TerminalSession::~TerminalSession()
{
    if (!d->active) {
        return;
    }

    write(QStringLiteral("\x1b[0m\x1b[?25h\x1b[?1049l"));

#ifdef Q_OS_WIN
    SetConsoleMode(d->input, d->inputMode);
    SetConsoleMode(d->output, d->outputMode);
#elif defined(Q_OS_UNIX)
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &d->inputMode);
#endif
    d->active = false;
}

bool TerminalSession::isInteractive()
{
#ifdef Q_OS_WIN
    DWORD inputMode = 0;
    DWORD outputMode = 0;
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    return input != INVALID_HANDLE_VALUE && output != INVALID_HANDLE_VALUE
            && GetConsoleMode(input, &inputMode) && GetConsoleMode(output, &outputMode);
#elif defined(Q_OS_UNIX)
    return ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO);
#else
    return false;
#endif
}

bool TerminalSession::open()
{
    if (d->active || !isInteractive()) {
        return d->active;
    }

#ifdef Q_OS_WIN
    d->input = GetStdHandle(STD_INPUT_HANDLE);
    d->output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(d->input, &d->inputMode) || !GetConsoleMode(d->output, &d->outputMode)) {
        return false;
    }

    DWORD inputMode = d->inputMode;
    inputMode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE);
    inputMode |= ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT;
    if (!SetConsoleMode(d->input, inputMode)
        || !SetConsoleMode(d->output, d->outputMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        SetConsoleMode(d->input, d->inputMode);
        return false;
    }
    SetConsoleOutputCP(CP_UTF8);
#elif defined(Q_OS_UNIX)
    if (tcgetattr(STDIN_FILENO, &d->inputMode) != 0) {
        return false;
    }
    termios raw = d->inputMode;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return false;
    }
#else
    return false;
#endif

    d->active = true;
    write(QStringLiteral("\x1b[?1049h\x1b[?25l"));
    return true;
}

void TerminalSession::write(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    if (bytes.isEmpty()) {
        return;
    }

#ifdef Q_OS_WIN
    DWORD written = 0;
    WriteFile(d->output, bytes.constData(), static_cast<DWORD>(bytes.size()), &written, nullptr);
#elif defined(Q_OS_UNIX)
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(STDOUT_FILENO, bytes.constData() + offset,
                                        static_cast<size_t>(bytes.size() - offset));
        if (written > 0) {
            offset += written;
        } else if (written < 0 && errno != EINTR) {
            break;
        }
    }
#else
    Q_UNUSED(bytes);
#endif
}

TerminalSize TerminalSession::size() const
{
    TerminalSize result;
#ifdef Q_OS_WIN
    CONSOLE_SCREEN_BUFFER_INFO info {};
    if (GetConsoleScreenBufferInfo(d->output, &info)) {
        result.columns = info.srWindow.Right - info.srWindow.Left + 1;
        result.rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#elif defined(Q_OS_UNIX)
    winsize window {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0) {
        if (window.ws_col > 0) {
            result.columns = window.ws_col;
        }
        if (window.ws_row > 0) {
            result.rows = window.ws_row;
        }
    }
#endif
    result.columns = std::max(result.columns, 1);
    result.rows = std::max(result.rows, 1);
    return result;
}

#ifdef Q_OS_WIN
TerminalKey TerminalSession::readKey(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();

    for (;;) {
        const int remaining = std::max(0, timeoutMs - static_cast<int>(timer.elapsed()));
        const DWORD waitResult = WaitForSingleObject(d->input, static_cast<DWORD>(remaining));
        if (waitResult != WAIT_OBJECT_0) {
            return { TerminalKeyType::Timeout, {} };
        }

        INPUT_RECORD record {};
        DWORD count = 0;
        if (!ReadConsoleInputW(d->input, &record, 1, &count) || count == 0) {
            return { TerminalKeyType::Unknown, {} };
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) {
            if (timer.elapsed() >= timeoutMs) {
                return { TerminalKeyType::Timeout, {} };
            }
            continue;
        }

        const KEY_EVENT_RECORD &event = record.Event.KeyEvent;
        const bool control = (event.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        if (control && event.wVirtualKeyCode == 'C') return { TerminalKeyType::CtrlC, {} };
        if (control && event.wVirtualKeyCode == 'D') return { TerminalKeyType::CtrlD, {} };
        if (control && event.wVirtualKeyCode == 'L') return { TerminalKeyType::CtrlL, {} };

        switch (event.wVirtualKeyCode) {
        case VK_RETURN: return { TerminalKeyType::Enter, {} };
        case VK_BACK: return { TerminalKeyType::Backspace, {} };
        case VK_DELETE: return { TerminalKeyType::Delete, {} };
        case VK_LEFT: return { TerminalKeyType::Left, {} };
        case VK_RIGHT: return { TerminalKeyType::Right, {} };
        case VK_UP: return { TerminalKeyType::Up, {} };
        case VK_DOWN: return { TerminalKeyType::Down, {} };
        case VK_HOME: return { TerminalKeyType::Home, {} };
        case VK_END: return { TerminalKeyType::End, {} };
        case VK_PRIOR: return { TerminalKeyType::PageUp, {} };
        case VK_NEXT: return { TerminalKeyType::PageDown, {} };
        case VK_TAB: return { TerminalKeyType::Tab, {} };
        case VK_ESCAPE: return { TerminalKeyType::Escape, {} };
        default: break;
        }

        if (event.uChar.UnicodeChar >= 0x20) {
            return { TerminalKeyType::Text, QString(QChar(event.uChar.UnicodeChar)) };
        }
        return { TerminalKeyType::Unknown, {} };
    }
}
#elif defined(Q_OS_UNIX)
namespace
{
enum class ByteReadResult
{
    Value,
    Timeout,
    Closed
};

ByteReadResult readByte(unsigned char &value, int timeoutMs)
{
    pollfd descriptor { STDIN_FILENO, POLLIN, 0 };
    const int pollResult = ::poll(&descriptor, 1, timeoutMs);
    if (pollResult == 0 || (pollResult < 0 && errno == EINTR)) {
        return ByteReadResult::Timeout;
    }
    if (pollResult < 0 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
        return ByteReadResult::Closed;
    }

    const ssize_t readResult = ::read(STDIN_FILENO, &value, 1);
    if (readResult == 1) {
        return ByteReadResult::Value;
    }
    if (readResult < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return ByteReadResult::Timeout;
    }
    return ByteReadResult::Closed;
}
} // namespace

TerminalKey TerminalSession::readKey(int timeoutMs)
{
    unsigned char first = 0;
    const ByteReadResult firstResult = readByte(first, timeoutMs);
    if (firstResult == ByteReadResult::Timeout) {
        return { TerminalKeyType::Timeout, {} };
    }
    if (firstResult == ByteReadResult::Closed) {
        return { TerminalKeyType::CtrlD, {} };
    }

    switch (first) {
    case 3: return { TerminalKeyType::CtrlC, {} };
    case 4: return { TerminalKeyType::CtrlD, {} };
    case 8:
    case 127: return { TerminalKeyType::Backspace, {} };
    case 9: return { TerminalKeyType::Tab, {} };
    case 10:
    case 13: return { TerminalKeyType::Enter, {} };
    case 12: return { TerminalKeyType::CtrlL, {} };
    default: break;
    }

    if (first == 0x1b) {
        QByteArray sequence;
        unsigned char next = 0;
        while (sequence.size() < 8 && readByte(next, 8) == ByteReadResult::Value) {
            sequence.append(static_cast<char>(next));
            if (((next >= 'A' && next <= 'Z') && sequence.size() > 1) || next == '~') {
                break;
            }
        }
        if (sequence.isEmpty()) return { TerminalKeyType::Escape, {} };
        if (sequence == "[A") return { TerminalKeyType::Up, {} };
        if (sequence == "[B") return { TerminalKeyType::Down, {} };
        if (sequence == "[C") return { TerminalKeyType::Right, {} };
        if (sequence == "[D") return { TerminalKeyType::Left, {} };
        if (sequence == "OA") return { TerminalKeyType::Up, {} };
        if (sequence == "OB") return { TerminalKeyType::Down, {} };
        if (sequence == "OC") return { TerminalKeyType::Right, {} };
        if (sequence == "OD") return { TerminalKeyType::Left, {} };
        if (sequence == "[H" || sequence == "[1~" || sequence == "[7~") return { TerminalKeyType::Home, {} };
        if (sequence == "[F" || sequence == "[4~" || sequence == "[8~") return { TerminalKeyType::End, {} };
        if (sequence == "OH") return { TerminalKeyType::Home, {} };
        if (sequence == "OF") return { TerminalKeyType::End, {} };
        if (sequence == "[3~") return { TerminalKeyType::Delete, {} };
        if (sequence == "[5~") return { TerminalKeyType::PageUp, {} };
        if (sequence == "[6~") return { TerminalKeyType::PageDown, {} };
        return { TerminalKeyType::Unknown, {} };
    }

    int byteCount = 1;
    if ((first & 0xe0) == 0xc0) byteCount = 2;
    else if ((first & 0xf0) == 0xe0) byteCount = 3;
    else if ((first & 0xf8) == 0xf0) byteCount = 4;

    QByteArray bytes(1, static_cast<char>(first));
    while (bytes.size() < byteCount) {
        unsigned char next = 0;
        if (readByte(next, 20) != ByteReadResult::Value) {
            break;
        }
        bytes.append(static_cast<char>(next));
    }
    const QString text = QString::fromUtf8(bytes);
    return text.isEmpty() ? TerminalKey { TerminalKeyType::Unknown, {} }
                          : TerminalKey { TerminalKeyType::Text, text };
}
#else
TerminalKey TerminalSession::readKey(int timeoutMs)
{
    Q_UNUSED(timeoutMs);
    return { TerminalKeyType::Timeout, {} };
}
#endif

QString clipAnsiLine(const QString &line, int width)
{
    if (width <= 0) {
        return {};
    }

    QString result;
    result.reserve(line.size());
    int visible = 0;
    bool truncated = false;

    for (int i = 0; i < line.size();) {
        if (line.at(i) == QChar(0x1b) && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('[')) {
            const int start = i;
            i += 2;
            while (i < line.size()) {
                const ushort value = line.at(i).unicode();
                ++i;
                if (value >= 0x40 && value <= 0x7e) {
                    break;
                }
            }
            result.append(line.mid(start, i - start));
            continue;
        }

        if (line.at(i) == QLatin1Char('\r') || line.at(i) == QLatin1Char('\n')) {
            ++i;
            continue;
        }
        if (visible >= width) {
            truncated = true;
            break;
        }
        result.append(line.at(i));
        ++visible;
        ++i;
    }

    if (truncated) {
        result.append(QStringLiteral("\x1b[0m"));
    }
    return result;
}

TerminalRenderer::TerminalRenderer(TerminalSession &terminal) : m_terminal(terminal) {}

void TerminalRenderer::invalidate()
{
    m_previousSize = { 0, 0 };
    m_previousLines.clear();
    m_cursorRow = -1;
    m_cursorColumn = -1;
    m_cursorVisible = false;
}

void TerminalRenderer::render(const QStringList &lines, int cursorRow, int cursorColumn)
{
    const TerminalSize size = m_terminal.size();
    const bool fullRender = size != m_previousSize;
    QStringList nextLines;
    nextLines.reserve(size.rows);
    for (int row = 0; row < size.rows; ++row) {
        nextLines.append(clipAnsiLine(lines.value(row), size.columns));
    }

    QString update;
    if (fullRender) {
        update.append(QStringLiteral("\x1b[2J"));
    }
    for (int row = 0; row < size.rows; ++row) {
        if (fullRender || m_previousLines.value(row) != nextLines.at(row)) {
            update.append(QStringLiteral("\x1b[%1;1H\x1b[2K%2").arg(row + 1).arg(nextLines.at(row)));
        }
    }

    if (cursorRow > 0 && cursorColumn > 0) {
        cursorRow = std::min(std::max(cursorRow, 1), size.rows);
        cursorColumn = std::min(std::max(cursorColumn, 1), size.columns);
        if (fullRender || cursorRow != m_cursorRow || cursorColumn != m_cursorColumn) {
            update.append(QStringLiteral("\x1b[%1;%2H").arg(cursorRow).arg(cursorColumn));
        }
        if (fullRender || !m_cursorVisible) {
            update.append(QStringLiteral("\x1b[?25h"));
        }
    } else {
        if (fullRender || m_cursorVisible) {
            update.append(QStringLiteral("\x1b[?25l"));
        }
    }

    if (!update.isEmpty()) {
        m_terminal.write(update);
    }
    m_previousSize = size;
    m_previousLines = std::move(nextLines);
    m_cursorRow = cursorRow;
    m_cursorColumn = cursorColumn;
    m_cursorVisible = cursorRow > 0 && cursorColumn > 0;
}
} // namespace amnezia::cli
