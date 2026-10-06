#include <stdio.h>
#include <windows.h>

int main(void)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(out, &info)) {
        printf("not a console\n");
        return 1;
    }
    WORD saved = info.wAttributes;
    printf("console %dx%d\n", info.dwSize.X, info.dwSize.Y);
    const WORD colors[] = {FOREGROUND_RED | FOREGROUND_INTENSITY, FOREGROUND_GREEN | FOREGROUND_INTENSITY,
                           FOREGROUND_BLUE | FOREGROUND_INTENSITY, FOREGROUND_RED | FOREGROUND_GREEN,
                           BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY};
    const char *names[] = {"red", "green", "blue", "yellow", "white on blue"};
    for (int i = 0; i < 5; i++) {
        SetConsoleTextAttribute(out, colors[i]);
        printf(" %s ", names[i]);
    }
    SetConsoleTextAttribute(out, saved);
    printf("\n");
    DWORD n;
    WriteConsoleW(out, L"Unicode: \x041f\x0440\x0438\x0432\x0435\x0442 \x2192 ok\n", 16, &n, NULL);
    SetConsoleTitleA("SynthOS console demo");
    return 0;
}
