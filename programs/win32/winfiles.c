#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

static const CLSID open_dialog = {0xdc1c5a9c, 0xe88a, 0x4dde, {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};
static const IID open_dialog_iid = {0xd57c7288, 0xd4ad, 0x4768, {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};
static const IID shell_item_iid = {0x43826d1e, 0xe718, 0x42ee, {0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}};

int main(void)
{
    CreateDirectoryA("C:\\Temp\\wtest", NULL);
    FILE *f = fopen("C:\\Temp\\wtest\\notes.txt", "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    for (int i = 1; i <= 3; i++)
        fprintf(f, "line %d\n", i);
    fclose(f);

    f = fopen("c:\\temp\\WTEST\\NOTES.TXT", "r");
    char line[64];
    int count = 0;
    while (fgets(line, sizeof(line), f))
        count++;
    fseek(f, 0, SEEK_END);
    printf("lines=%d size=%ld\n", count, ftell(f));
    fclose(f);

    HANDLE h = CreateFileA("C:\\Temp\\wtest\\raw.bin", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD written;
    WriteFile(h, "SYNTH", 5, &written, NULL);
    CloseHandle(h);
    CopyFileA("C:\\Temp\\wtest\\raw.bin", "C:\\Temp\\wtest\\copy.bin", FALSE);

    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA("C:\\Temp\\wtest\\*", &fd);
    int files = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            printf("found %s (%lu bytes)\n", fd.cFileName, fd.nFileSizeLow);
            files++;
        }
    } while (FindNextFileA(find, &fd));
    FindClose(find);

    char cwd[MAX_PATH];
    SetCurrentDirectoryA("C:\\Temp\\wtest");
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    printf("cwd=%s files=%d\n", cwd, files);
    printf("attr=%lx missing=%lx err=%lu\n", GetFileAttributesA("copy.bin"), GetFileAttributesA("nope.txt"),
           GetLastError());

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IFileOpenDialog *dlg = NULL;
    HRESULT hr = CoCreateInstance(&open_dialog, NULL, CLSCTX_INPROC_SERVER, &open_dialog_iid, (void **)&dlg);
    DWORD opts = 0;
    if (SUCCEEDED(hr)) {
        dlg->lpVtbl->SetOptions(dlg, FOS_PICKFOLDERS);
        dlg->lpVtbl->GetOptions(dlg, &opts);
    }
    IShellItem *item = NULL;
    WCHAR *wide = NULL;
    char shown[MAX_PATH] = "-";
    if (SUCCEEDED(SHCreateItemFromParsingName(L"C:\\Temp\\wtest", NULL, &shell_item_iid, (void **)&item)) &&
        SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &wide)))
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, shown, sizeof(shown), NULL, NULL);
    if (dlg && item)
        dlg->lpVtbl->SetFolder(dlg, item);
    printf("file dialog hr=%lx options=%lx item=%s\n", (unsigned long)hr, opts, shown);
    OPENFILENAMEA ofn = {sizeof(ofn)};
    char file[MAX_PATH] = "";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    printf("open dialog without desktop=%d\n", GetOpenFileNameA(&ofn));
    if (item)
        item->lpVtbl->Release(item);
    if (dlg)
        dlg->lpVtbl->Release(dlg);
    CoTaskMemFree(wide);

    DeleteFileA("copy.bin");
    return 0;
}
