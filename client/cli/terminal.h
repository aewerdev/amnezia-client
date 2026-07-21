#ifndef AMNEZIA_CLI_TERMINAL_H
#define AMNEZIA_CLI_TERMINAL_H

#include <memory>

#include <QString>
#include <QStringList>

namespace amnezia::cli
{
enum class TerminalKeyType
{
    Timeout,
    Text,
    Enter,
    Backspace,
    Delete,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    Tab,
    Escape,
    CtrlC,
    CtrlD,
    CtrlL,
    Unknown
};

struct TerminalKey
{
    TerminalKeyType type = TerminalKeyType::Unknown;
    QString text;
};

struct TerminalSize
{
    int columns = 80;
    int rows = 24;

    bool operator==(const TerminalSize &other) const
    {
        return columns == other.columns && rows == other.rows;
    }

    bool operator!=(const TerminalSize &other) const { return !(*this == other); }
};

class TerminalSession
{
public:
    TerminalSession();
    ~TerminalSession();

    TerminalSession(const TerminalSession &) = delete;
    TerminalSession &operator=(const TerminalSession &) = delete;

    static bool isInteractive();
    bool open();
    TerminalKey readKey(int timeoutMs);
    TerminalSize size() const;
    void write(const QString &text);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

QString clipAnsiLine(const QString &line, int width);

class TerminalRenderer
{
public:
    explicit TerminalRenderer(TerminalSession &terminal);

    void invalidate();
    void render(const QStringList &lines, int cursorRow = -1, int cursorColumn = -1);

private:
    TerminalSession &m_terminal;
    TerminalSize m_previousSize { 0, 0 };
    QStringList m_previousLines;
    int m_cursorRow = -1;
    int m_cursorColumn = -1;
    bool m_cursorVisible = false;
};
} // namespace amnezia::cli

#endif // AMNEZIA_CLI_TERMINAL_H
