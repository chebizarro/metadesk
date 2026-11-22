#include "management_ipc.h"
#include "md_test.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static cJSON *call(const char *path,const char *method) {
    cJSON *j=cJSON_CreateObject();cJSON_AddNumberToObject(j,"v",1);cJSON_AddStringToObject(j,"method",method);
    cJSON *r=md_management_call(path,j);cJSON_Delete(j);return r;
}
int main(void) {
    int pair[2];MD_CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    MD_CHECK(md_management_peer_is_user(pair[0],(uint64_t)geteuid()));
    MD_CHECK(!md_management_peer_is_user(pair[0],(uint64_t)geteuid()+1));
    MD_CHECK(!md_management_peer_is_user(-1,(uint64_t)geteuid()));close(pair[0]);close(pair[1]);
    char dir[]="/private/tmp/md-w13-ipc-XXXXXX";
#ifdef __linux__
    strcpy(dir,"/tmp/md-w13-ipc-XXXXXX");
#endif
    MD_CHECK(mkdtemp(dir));char path[108];snprintf(path,sizeof(path),"%s/host.sock",dir);
    MdHostAccess a;MD_CHECK(!md_host_access_init(&a));
    MdManagementIpc *server=md_management_start(&a,NULL,NULL,path,7,NULL,NULL);MD_CHECK(server);
    MD_CHECK(!md_management_start(&a,NULL,NULL,path,7,NULL,NULL)); // cannot replace live endpoint
    cJSON *v=call(path,"management.snapshot");MD_CHECK(v&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v,"ok")));cJSON_Delete(v);
    md_host_access_lifecycle(&a,true,"waiting");
    v=call(path,"management.snapshot");
    cJSON *host=cJSON_GetObjectItemCaseSensitive(v,"host");
    MD_CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(host,"managed")));
    MD_CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(host,"state")->valuestring,"waiting"));
    cJSON_Delete(v);
    v=call(path,"arbitrary.signer");MD_CHECK(v&&cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(v,"ok")));cJSON_Delete(v);
    struct stat st;MD_CHECK(!lstat(path,&st));MD_CHECK((st.st_mode&0777)==0600);
    // A half-close is a semantic EOF barrier: idle subscribers must release
    // their worker without requiring another state mutation (no sleeps).
    for (unsigned i=0; i<16; ++i) {
        int fd=socket(AF_UNIX,SOCK_STREAM,0);MD_CHECK(fd>=0);
        struct timeval timeout={.tv_sec=5};
        MD_CHECK(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
        struct sockaddr_un addr={.sun_family=AF_UNIX};strcpy(addr.sun_path,path);
        MD_CHECK(!connect(fd,(struct sockaddr *)&addr,sizeof(addr)));
        const char *line="{\"v\":1,\"method\":\"management.subscribe\"}\n";
        MD_CHECK(send(fd,line,strlen(line),0)==(ssize_t)strlen(line));
        char ch=0;do { MD_CHECK(recv(fd,&ch,1,0)==1); } while(ch!='\n');
        MD_CHECK(!shutdown(fd,SHUT_WR));MD_CHECK(recv(fd,&ch,1,0)==0);close(fd);
    }
    // Symlink and unsafe parent rejection, also on client.
    char link[108];snprintf(link,sizeof(link),"%s/link",dir);MD_CHECK(!symlink(path,link));
    MD_CHECK(!call(link,"management.snapshot"));unlink(link);
    MD_CHECK(!chmod(dir,0755));MD_CHECK(!call(path,"management.snapshot"));MD_CHECK(!chmod(dir,0700));
    md_management_stop(server);MD_CHECK(lstat(path,&st)<0);md_host_access_destroy(&a);
    MD_CHECK(!md_host_access_init(&a));
    FILE *regular=fopen(path,"wx");MD_CHECK(regular);fclose(regular);
    MD_CHECK(!md_management_start_managed(&a,NULL,NULL,path,7,NULL,NULL));
    MD_CHECK(!lstat(path,&st)&&S_ISREG(st.st_mode));MD_CHECK(!unlink(path));
    MD_CHECK(!symlink("missing",path));
    MD_CHECK(!md_management_start_managed(&a,NULL,NULL,path,7,NULL,NULL));
    MD_CHECK(!lstat(path,&st)&&S_ISLNK(st.st_mode));MD_CHECK(!unlink(path));
    int stale=socket(AF_UNIX,SOCK_STREAM,0);MD_CHECK(stale>=0);
    struct sockaddr_un addr={.sun_family=AF_UNIX};strcpy(addr.sun_path,path);
    MD_CHECK(!bind(stale,(struct sockaddr *)&addr,sizeof(addr)));close(stale);MD_CHECK(!chmod(path,0600));
    server=md_management_start_managed(&a,NULL,NULL,path,7,NULL,NULL);MD_CHECK(server);
    MD_CHECK(!md_management_start_managed(&a,NULL,NULL,path,7,NULL,NULL));
    md_management_stop(server);MD_CHECK(lstat(path,&st)<0);
    char lock[120];snprintf(lock,sizeof(lock),"%s.lock",path);MD_CHECK(!unlink(lock));
    md_host_access_destroy(&a);MD_CHECK(!rmdir(dir));
    puts("PASS: real kernel peer UID accepted/rejected, invalid fd rejected, private endpoint, live-path/symlink/unsafe-parent refusal, cleanup");
}
