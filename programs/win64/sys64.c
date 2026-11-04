/* 64-bit layouts of system information: processors, UI language, class info. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

int main(void)
{
    DWORD len = 0;
    BOOL first = GetLogicalProcessorInformation(NULL, &len);
    DWORD err = GetLastError();
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info = malloc(len);
    BOOL ok = GetLogicalProcessorInformation(info, &len);
    int cores = 0, caches = 0, l2 = 0;
    for (DWORD i = 0; ok && i < len / sizeof(*info); i++) {
        cores += info[i].Relationship == RelationProcessorCore;
        if (info[i].Relationship == RelationCache) {
            caches++;
            if (info[i].Cache.Level == 2)
                l2 = (int)(info[i].Cache.Size / 1024);
        }
    }
    printf("cpuinfo first=%d err=%lu ok=%d stride=%d cores=%d caches=%d l2=%dk\n", first, err, ok,
           (int)(len % sizeof(*info)), cores, caches, l2);
    free(info);
    WNDCLASSW wc;
    BOOL cls = GetClassInfoW(NULL, L"Button", &wc);
    printf("uilang thread=%x class=%d proc=%d name=%d\n", GetThreadUILanguage(), cls, wc.lpfnWndProc != NULL,
           wc.lpszClassName != NULL);
    return 0;
}
