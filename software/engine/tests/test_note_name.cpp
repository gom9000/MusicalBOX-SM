#include "check.h"
#include "sampler/NoteName.h"

using namespace mbx;

int main() {
    CHECK(parseNoteName("C3") == 60);
    CHECK(parseNoteName("c3") == 60);
    CHECK(parseNoteName("C#3") == 61);
    CHECK(parseNoteName("Db3") == 61);
    CHECK(parseNoteName("B2") == 59);
    CHECK(parseNoteName("Bb2") == 58);
    CHECK(parseNoteName("A-1") == 21);
    CHECK(parseNoteName("C-2") == 0);
    CHECK(parseNoteName("G8") == 127);
    CHECK(parseNoteName("G#8") == -1);  // 128
    CHECK(parseNoteName("H3") == -1);
    CHECK(parseNoteName("C") == -1);
    CHECK(parseNoteName("C3x") == -1);
    CHECK(parseNoteName("") == -1);

    CHECK(noteName(60) == "C3");
    CHECK(noteName(61) == "C#3");
    CHECK(noteName(21) == "A-1");
    for (int n = 0; n < 128; ++n) CHECK(parseNoteName(noteName(n)) == n);

    return finish("test_note_name");
}
