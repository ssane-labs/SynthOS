/* Downloads a URL three ways: WinINet, WinHTTP and URLDownloadToFile. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <urlmon.h>
#include <wininet.h>

#define WINHTTP_ACCESS_TYPE_DEFAULT_PROXY 0
#define WINHTTP_FLAG_SECURE 0x00800000
#define WINHTTP_QUERY_STATUS_CODE 19
#define WINHTTP_QUERY_FLAG_NUMBER 0x20000000
HINTERNET WINAPI WinHttpOpen(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
HINTERNET WINAPI WinHttpConnect(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
HINTERNET WINAPI WinHttpOpenRequest(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
BOOL WINAPI WinHttpSendRequest(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
BOOL WINAPI WinHttpReceiveResponse(HINTERNET, LPVOID);
BOOL WINAPI WinHttpQueryHeaders(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
BOOL WINAPI WinHttpQueryDataAvailable(HINTERNET, LPDWORD);
BOOL WINAPI WinHttpReadData(HINTERNET, LPVOID, DWORD, LPDWORD);
BOOL WINAPI WinHttpCloseHandle(HINTERNET);

static void with_wininet(const char *url)
{
    HINTERNET net = InternetOpenA("winnet", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    HINTERNET req = InternetOpenUrlA(net, url, NULL, 0, INTERNET_FLAG_RELOAD, 0);
    if (!req) {
        printf("wininet: error %lu\n", GetLastError());
        InternetCloseHandle(net);
        return;
    }
    DWORD status = 0, size = sizeof(status);
    HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, NULL);
    char buf[256];
    DWORD got = 0, total = 0;
    char first[64] = "";
    while (InternetReadFile(req, buf, sizeof(buf) - 1, &got) && got) {
        if (!total) {
            buf[got] = 0;
            snprintf(first, sizeof(first), "%.*s", (int)strcspn(buf, "\r\n"), buf);
        }
        total += got;
    }
    printf("wininet: status %lu, %lu bytes, first line '%s'\n", status, total, first);
    InternetCloseHandle(req);
    InternetCloseHandle(net);
}

static void with_winhttp(const wchar_t *host, INTERNET_PORT port, const wchar_t *path, BOOL secure)
{
    HINTERNET s = WinHttpOpen(L"winnet", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    HINTERNET c = WinHttpConnect(s, host, port, 0);
    HINTERNET r = WinHttpOpenRequest(c, L"GET", path, NULL, NULL, NULL, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!WinHttpSendRequest(r, NULL, 0, NULL, 0, 0, 0) || !WinHttpReceiveResponse(r, NULL)) {
        printf("winhttp: error %lu\n", GetLastError());
    } else {
        DWORD status = 0, size = sizeof(status), avail = 0, total = 0, got;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL);
        char buf[512];
        while (WinHttpQueryDataAvailable(r, &avail) && avail) {
            WinHttpReadData(r, buf, avail < sizeof(buf) ? avail : sizeof(buf), &got);
            total += got;
        }
        printf("winhttp: status %lu, %lu bytes\n", status, total);
    }
    WinHttpCloseHandle(r);
    WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
}

int main(int argc, char **argv)
{
    const char *url = argc > 1 ? argv[1] : "http://10.0.2.2:8000/";
    with_wininet(url);
    wchar_t host[256], path[512];
    char h[256] = "", p[512] = "/";
    int port = 0;
    BOOL secure = strncmp(url, "https://", 8) == 0;
    sscanf(url + (secure ? 8 : 7), "%255[^:/]:%d%511s", h, &port, p);
    if (!port) {
        port = secure ? 443 : 80;
        sscanf(url + (secure ? 8 : 7), "%255[^/]%511s", h, p);
    }
    MultiByteToWideChar(CP_UTF8, 0, h, -1, host, 256);
    MultiByteToWideChar(CP_UTF8, 0, p, -1, path, 512);
    with_winhttp(host, (INTERNET_PORT)port, path, secure);
    HRESULT hr = URLDownloadToFileA(NULL, url, "download.bin", 0, NULL);
    HANDLE f = CreateFileA("download.bin", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    printf("urlmon: %s, %lu bytes\n", hr == S_OK ? "ok" : "failed", f != INVALID_HANDLE_VALUE ? GetFileSize(f, NULL) : 0);
    CloseHandle(f);
    return 0;
}
