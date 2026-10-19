#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

static const CLSID open_dialog = {0xdc1c5a9c, 0xe88a, 0x4dde, {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};
static const IID open_dialog_iid = {0xd57c7288, 0xd4ad, 0x4768, {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};
static const IID shell_item_iid = {0x43826d1e, 0xe718, 0x42ee, {0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}};
static const CLSID file_operation = {0x3ad05575, 0x8857, 0x4850, {0x92, 0x77, 0x11, 0xb8, 0x5b, 0xdb, 0x8e, 0x09}};
static const IID file_operation_iid = {0x947aab5f, 0x0a5c, 0x4c13, {0xb4, 0xd6, 0x4b, 0xf7, 0x83, 0x6f, 0xc9, 0xf8}};

/* Chromium renames every finished download with IFileOperation and tags it with a Zone.Identifier stream. */
static void file_operation_test(void)
{
    CreateDirectoryA("C:\\Temp\\wtest\\done", NULL);
    HANDLE h = CreateFileA("C:\\Temp\\wtest\\part.tmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD written;
    WriteFile(h, "download", 8, &written, NULL);
    CloseHandle(h);
    h = CreateFileA("C:\\Temp\\wtest\\part.tmp:Zone.Identifier", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    BOOL stream = h != INVALID_HANDLE_VALUE && WriteFile(h, "[ZoneTransfer]", 14, &written, NULL);
    CloseHandle(h);
    h = CreateFileA("C:\\Temp\\wtest\\part.tmp:Zone.Identifier", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    printf("stream write=%d read=%d\n", stream, h != INVALID_HANDLE_VALUE);
    IFileOperation *op = NULL;
    IShellItem *source = NULL, *folder = NULL;
    BOOL aborted = TRUE;
    HRESULT hr = CoCreateInstance(&file_operation, NULL, CLSCTX_ALL, &file_operation_iid, (void **)&op);
    if (SUCCEEDED(hr))
        hr = SHCreateItemFromParsingName(L"C:\\Temp\\wtest\\part.tmp", NULL, &shell_item_iid, (void **)&source);
    if (SUCCEEDED(hr))
        hr = SHCreateItemFromParsingName(L"C:\\Temp\\wtest\\done", NULL, &shell_item_iid, (void **)&folder);
    if (SUCCEEDED(hr))
        hr = op->lpVtbl->MoveItem(op, source, folder, L"file.exe", NULL);
    if (SUCCEEDED(hr))
        hr = op->lpVtbl->PerformOperations(op);
    if (op)
        op->lpVtbl->GetAnyOperationsAborted(op, &aborted);
    printf("file operation hr=%lx aborted=%d moved=%d left=%d\n", (unsigned long)hr, aborted,
           GetFileAttributesA("C:\\Temp\\wtest\\done\\file.exe") != INVALID_FILE_ATTRIBUTES,
           GetFileAttributesA("C:\\Temp\\wtest\\part.tmp") != INVALID_FILE_ATTRIBUTES);
    if (source)
        source->lpVtbl->Release(source);
    if (folder)
        folder->lpVtbl->Release(folder);
    if (op)
        op->lpVtbl->Release(op);
    DeleteFileA("C:\\Temp\\wtest\\done\\file.exe");
    RemoveDirectoryA("C:\\Temp\\wtest\\done");
}

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
    file_operation_test();

    DeleteFileA("copy.bin");
    return 0;
}
