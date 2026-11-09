#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char (WINAPI *OpenStorage_t)(const void*, DWORD, HANDLE*);
typedef HANDLE (WINAPI *FindFirst_t)(HANDLE, const char*, void*, const void*);
typedef unsigned char (WINAPI *FindNext_t)(HANDLE, void*);
typedef unsigned char (WINAPI *OpenFile_t)(HANDLE, const void*, DWORD, DWORD, HANDLE*);
typedef unsigned char (WINAPI *ReadFile_t)(HANDLE, void*, DWORD, DWORD*);
typedef unsigned char (WINAPI *CloseH_t)(HANDLE);
static void mkdirs(char* p){ for(char* c=p;*c;c++){ if(*c=='/'||*c=='\\'){ char s=*c; *c=0; CreateDirectoryA(p,NULL); *c=s; } } }
/* usage: cx.exe <CascLib.dll> <storage> list <mask> | get <outdir> <file>... (files from stdin if none) */
int main(int argc,char**argv){
  HMODULE h=LoadLibraryA(argv[1]); if(!h){printf("no dll %lu\n",GetLastError());return 1;}
  OpenStorage_t os=(OpenStorage_t)GetProcAddress(h,"CascOpenStorage");
  FindFirst_t ff=(FindFirst_t)GetProcAddress(h,"CascFindFirstFile");
  FindNext_t fn=(FindNext_t)GetProcAddress(h,"CascFindNextFile");
  OpenFile_t of=(OpenFile_t)GetProcAddress(h,"CascOpenFile");
  ReadFile_t rf=(ReadFile_t)GetProcAddress(h,"CascReadFile");
  CloseH_t cf=(CloseH_t)GetProcAddress(h,"CascCloseFile");
  HANDLE st=0; WCHAR w[1024]; MultiByteToWideChar(CP_ACP,0,argv[2],-1,w,1024);
  if(!os(w,0,&st) && !os(argv[2],0,&st)){printf("open storage failed %lu\n",GetLastError());return 2;}
  if(!strcmp(argv[3],"list")){
    static char fd[4096]; HANDLE f=ff(st,strcmp(argv[4],"NULL")?argv[4]:NULL,fd,NULL);
    if(!f||f==INVALID_HANDLE_VALUE){printf("find failed %lu\n",GetLastError());return 3;}
    do{ puts(fd);}while(fn(f,fd));
    return 0;
  }
  char line[1024]; int i=5;
  for(;;){
    char* name;
    if(argc>5){ if(i>=argc)break; name=argv[i++]; } else { if(!fgets(line,1024,stdin))break; line[strcspn(line,"\r\n")]=0; name=line; }
    HANDLE f=0; if(!of(st,name,0,0,&f)){fprintf(stderr,"open fail %s %lu\n",name,GetLastError());continue;}
    char out[2048]; const char* rel=name; if(!strncmp(rel,"data:",5))rel+=5;
    snprintf(out,2048,"%s\\%s",argv[4],rel); mkdirs(out);
    HANDLE o=CreateFileA(out,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    static char buf[1<<20]; DWORD n=0,wr;
    for(;;){ n=0; unsigned char ok=rf(f,buf,sizeof buf,&n); if(n)WriteFile(o,buf,n,&wr,NULL); if(!ok||n<sizeof buf)break; } CloseHandle(o); cf(f);
  }
  return 0;
}
