// [PERF] The console's line splitting and tokenizing without string streams.
//
// OldConsole::executeLine split a script with std::istringstream::getline, and handleCommand
// built up to three more istringstreams per line (the first token, the keyword, the binding and
// its arguments). Under the MSVC runtime each construction costs about a microsecond, which over
// the 2.6 million lines of a PR level load came to some 4.5 s. These two classes scan the
// std::string directly and reproduce what those streams did, including their state bits:
//
//   ConsoleLineReader::readToken  operator>>(std::string&): skip isspace (classic locale), read
//                                 up to the next isspace; a stream already failed or at its end
//                                 sets failbit and leaves the string alone; reaching the end
//                                 sets eofbit.
//   ConsoleLineReader::skipWs     >> std::ws: needs a good stream (else failbit), skips isspace,
//                                 sets eofbit (only) at the end.
//   ConsoleLineReader::get        get(): needs a good stream (else failbit, EOF); at the end sets
//                                 eofbit and failbit and returns EOF; else the character as an
//                                 unsigned value.
//   ConsoleLineSplitter::next     getline(buf, 1023, '\n'): stores up to 1022 characters; a line
//                                 of 1023 or more stops the whole split (failbit), as does an
//                                 attempt at the end with nothing left; a NUL inside a line cuts
//                                 the line where the caller builds std::string(buf).
//
// BF2_PERF_CONPARSE=0 runs both on real istringstreams again.

#ifndef BF2_IO_CONSOLE_CONSOLELINEREADER_H
#define BF2_IO_CONSOLE_CONSOLELINEREADER_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

namespace dice {
namespace hfe {
namespace io {

inline bool consoleFastParseEnabled()
{
    static int on = -1;
    if (on < 0) { const char* e = std::getenv("BF2_PERF_CONPARSE"); on = (e && *e == '0') ? 0 : 1; }
    return on == 1;
}

// [PERF] BF2_PERF_CONDISPATCH=0: handleCommand compares every first token against its keywords
// again (see there), and ConsoleObjects searches its map for every binding call.
inline bool consoleFastDispatchEnabled()
{
    static int on = -1;
    if (on < 0) { const char* e = std::getenv("BF2_PERF_CONDISPATCH"); on = (e && *e == '0') ? 0 : 1; }
    return on == 1;
}

class ConsoleLineReader
{
public:
    // text must outlive the reader.
    explicit ConsoleLineReader(const std::string& text, bool fast = consoleFastParseEnabled())
        : m_text(text), m_pos(0), m_eof(false), m_fail(false)
    {
        if (!fast)
            m_stream.reset(new std::istringstream(text));
    }

    bool readToken(std::string& out)
    {
        if (m_stream)
            return static_cast<bool>(*m_stream >> out);
        if (m_eof || m_fail) { m_fail = true; return false; }
        const std::size_t size = m_text.size();
        while (m_pos < size && isSpace(m_text[m_pos]))
            ++m_pos;
        if (m_pos == size) { m_eof = true; m_fail = true; return false; }
        const std::size_t start = m_pos;
        while (m_pos < size && !isSpace(m_text[m_pos]))
            ++m_pos;
        out.assign(m_text, start, m_pos - start);
        if (m_pos == size)
            m_eof = true;
        return true;
    }

    void skipWs()
    {
        if (m_stream) { *m_stream >> std::ws; return; }
        if (m_eof || m_fail) { m_fail = true; return; }
        const std::size_t size = m_text.size();
        while (m_pos < size && isSpace(m_text[m_pos]))
            ++m_pos;
        if (m_pos == size)
            m_eof = true;
    }

    int get()
    {
        if (m_stream)
            return m_stream->get();
        if (m_eof || m_fail) { m_fail = true; return EOF; }
        if (m_pos == m_text.size()) { m_eof = true; m_fail = true; return EOF; }
        return static_cast<unsigned char>(m_text[m_pos++]);
    }

    bool eof() const { return m_stream ? m_stream->eof() : m_eof; }
    bool fail() const { return m_stream ? m_stream->fail() : m_fail; }

private:
    // The classic locale's isspace for char.
    static bool isSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
    }

    ConsoleLineReader(const ConsoleLineReader&);
    ConsoleLineReader& operator=(const ConsoleLineReader&);

    const std::string&                  m_text;
    std::size_t                         m_pos;
    bool                                m_eof;
    bool                                m_fail;
    std::unique_ptr<std::istringstream> m_stream;   // BF2_PERF_CONPARSE=0
};

class ConsoleLineSplitter
{
public:
    // text must outlive the splitter.
    explicit ConsoleLineSplitter(const std::string& text, bool fast = consoleFastParseEnabled())
        : m_text(text), m_pos(0), m_done(false)
    {
        if (!fast)
            m_stream.reset(new std::istringstream(text));
    }

    // Fills buf (at least 1023 bytes) with the next line, NUL-terminated; false when the split
    // ends, exactly where `while (stream.getline(buf, 1023, '\n'))` would stop.
    bool next(char* buf)
    {
        if (m_stream)
            return static_cast<bool>(m_stream->getline(buf, 1023, '\n'));
        if (m_done)
            return false;
        const std::size_t size = m_text.size();
        if (m_pos >= size) { m_done = true; buf[0] = 0; return false; }
        const std::size_t newline = m_text.find('\n', m_pos);
        const std::size_t end = newline == std::string::npos ? size : newline;
        if (end - m_pos > 1022)
        {
            // getline stores 1022 characters, finds neither the delimiter nor the end, and fails.
            std::memcpy(buf, m_text.data() + m_pos, 1022);
            buf[1022] = 0;
            m_done = true;
            return false;
        }
        std::memcpy(buf, m_text.data() + m_pos, end - m_pos);
        buf[end - m_pos] = 0;
        m_pos = newline == std::string::npos ? size : newline + 1;
        return true;
    }

private:
    ConsoleLineSplitter(const ConsoleLineSplitter&);
    ConsoleLineSplitter& operator=(const ConsoleLineSplitter&);

    const std::string&                  m_text;
    std::size_t                         m_pos;
    bool                                m_done;
    std::unique_ptr<std::istringstream> m_stream;   // BF2_PERF_CONPARSE=0
};

} // namespace io
} // namespace hfe
} // namespace dice

#endif
