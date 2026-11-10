// Rules of the shared Generic (6.0) text renderer in print/PrintText.h.
#include "../print/PrintText.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace frond;
using Lines = std::vector<std::string>;

static int failures = 0;
#define CHECK(c) do { if(!(c)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while(0)

static PrintText r(const std::string &s) { return renderPrintText(reinterpret_cast<const uint8_t *>(s.data()), s.size()); }

int main() {
    // Line ends: CR, LF, CRLF (once) and a mixed run.
    CHECK(r("a\r\nb\r\n").pages == (std::vector<Lines>{{"a", "b"}}));
    CHECK(r("a\nb").pages == (std::vector<Lines>{{"a", "b"}}));
    CHECK(r("a\rb").pages == (std::vector<Lines>{{"a", "b"}}));
    CHECK(r("a\r\rb").pages == (std::vector<Lines>{{"a", "", "b"}}));
    CHECK(r("a\n\r").pages == (std::vector<Lines>{{"a", ""}}));
    CHECK(r("").pages.empty() && r("").renderable);

    // Form feed starts a new page; a trailing one adds none.
    CHECK(r("a\r\n\fb\r\n").pages == (std::vector<Lines>{{"a"}, {"b"}}));
    CHECK(r("a\fb").pages == (std::vector<Lines>{{"a"}, {"b"}}));
    CHECK(r("a\r\n\f").pages == (std::vector<Lines>{{"a"}}));
    CHECK(r("a\f\f").pages == (std::vector<Lines>{{"a"}, {}}));

    // Tab to the next multiple of 8.
    CHECK(r("a\tb").pages[0][0] == "a       b");
    CHECK(r("12345678\tb").pages[0][0] == "12345678        b");
    CHECK(r("\tx").pages[0][0] == "        x");

    // Wrap at 80 columns; exactly 80 does not add a line.
    CHECK(r(std::string(80, 'x') + "\r\n").pages[0] == Lines{std::string(80, 'x')});
    CHECK(r(std::string(81, 'x') + "\r\n").pages[0] == (Lines{std::string(80, 'x'), "x"}));
    CHECK(r(std::string(160, 'x')).pages[0].size() == 2);
    CHECK(r(std::string(80, 'x') + "\t|").pages[0] == (Lines{std::string(80, 'x'), "        |"}));

    // 66 lines per page; overflow starts a new page; exactly 66 does not.
    std::string s66, s67;
    for(int i = 0; i < 66; i++) s66 += "l\r\n";
    s67 = s66 + "last\r\n";
    CHECK(r(s66).pages.size() == 1 && r(s66).pages[0].size() == 66);
    CHECK(r(s66 + "\f").pages.size() == 1);
    CHECK(r(s67).pages.size() == 2 && r(s67).pages[1] == Lines{"last"});

    // Control bytes make the job not renderable.
    for(int c : {0x00, 0x01, 0x07, 0x08, 0x0b, 0x1b, 0x1f, 0x7f}) {
        PrintText t = r(std::string("ab") + char(c) + "cd");
        CHECK(!t.renderable && t.pages.empty() && !t.reason.empty());
    }
    CHECK(r("\x1b" "E").reason == "Only the Generic printer profile can be shown as pages");

    // High bytes: cp1252 as UTF-8.
    CHECK(r("\xe9").pages[0][0] == "\xc3\xa9");
    CHECK(r("\x80").pages[0][0] == "\xe2\x82\xac");
    CHECK(r("\x92").pages[0][0] == "\xe2\x80\x99");
    CHECK(r("\x81").pages[0][0] == "\xef\xbf\xbd");
    CHECK(r("a\xa0" "b").pages[0][0] == "a b");
    CHECK(r("\xff").pages[0][0] == "\xc3\xbf");
    CHECK(r(std::string(80, '\xe9')).pages[0].size() == 1);  // columns count characters, not UTF-8 bytes

    // TestPage shape: a few CRLF text lines and a closing form feed.
    PrintText t = r("Palm OS Test Page\r\n\r\nGeneric printer\r\n  tab\there\r\n\f");
    CHECK(t.renderable && t.pages.size() == 1);
    CHECK((t.pages[0] == Lines{"Palm OS Test Page", "", "Generic printer", "  tab   here"}));

    if(failures) return 1;
    std::puts("print-text ok");
    return 0;
}
