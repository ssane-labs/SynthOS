/* A dialog-based program built from a resource script. */
#include <ctype.h>
#include <stdio.h>
#include <windows.h>

#define IDD_MAIN 100
#define IDC_NAME 101
#define IDC_LOUD 102
#define IDC_GREET 103
#define IDC_RESULT 104
#define IDS_TITLE 200
#define IDS_HELLO 201

static char greeting[128];

static INT_PTR CALLBACK proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextA(dlg, IDC_NAME, "SynthOS");
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_GREET: {
            char name[64];
            GetDlgItemTextA(dlg, IDC_NAME, name, sizeof(name));
            snprintf(greeting, sizeof(greeting), "Hello, %s!", name);
            if (IsDlgButtonChecked(dlg, IDC_LOUD))
                for (char *p = greeting; *p; p++)
                    *p = (char)toupper((unsigned char)*p);
            SetDlgItemTextA(dlg, IDC_RESULT, greeting);
            return TRUE;
        }
        case IDOK:
        case IDCANCEL: EndDialog(dlg, LOWORD(wp)); return TRUE;
        }
    }
    return FALSE;
}

int main(void)
{
    char title[64], hello[64];
    HINSTANCE inst = GetModuleHandleA(NULL);
    LoadStringA(inst, IDS_TITLE, title, sizeof(title));
    LoadStringA(inst, IDS_HELLO, hello, sizeof(hello));
    printf("strings: %s / %s\n", title, hello);
    HRSRC res = FindResourceA(inst, MAKEINTRESOURCEA(IDD_MAIN), (LPCSTR)RT_DIALOG);
    printf("dialog resource: %s, %lu bytes\n", res ? "found" : "missing", res ? SizeofResource(inst, res) : 0);
    INT_PTR r = DialogBoxParamA(inst, MAKEINTRESOURCEA(IDD_MAIN), NULL, proc, 0);
    if (r == -1) {
        puts("dialog needs the SynthOS desktop");
        return 1;
    }
    printf("dialog closed with %s, last greeting: %s\n", r == IDOK ? "OK" : "Cancel", greeting);
    return 0;
}
