#include "source.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

int main(int argc,char** argv) {
    if(argc!=3) { std::fprintf(stderr,"Expected game_dir level; got %d arguments\n",argc); return 2; }
    int failed=0;
    for(int legacy=1;legacy>=0;--legacy) {
#ifdef _WIN32
        _putenv_s("BF6_MOUNT_LITERAL_PATH",legacy?"1":"0");
#else
        setenv("BF6_MOUNT_LITERAL_PATH",legacy?"1":"0",1);
#endif
        bf6::Source s;std::string err;
        if(!s.open(argv[1],err)) { std::fprintf(stderr,"open: %s\n",err.c_str());return 2; }
        std::string selected;
        for(const auto& path:s.find_tocs(argv[2],false)) {
            err.clear();
            if(s.mount_toc(path,err) && s.res_entries_total()>0) {selected=path;break;}
        }
        if(selected.empty()) { std::fprintf(stderr,"No resource archive selected: %s\n",err.c_str());return 2; }
        // Same existing file, alternate separators and a harmless dot segment.
        auto file=std::filesystem::path(selected);
        std::string alias=(file.parent_path()/"."/file.filename()).generic_string();
        auto before=s.res_entries_total();auto count=s.res_count();
        auto t=std::chrono::steady_clock::now();
        err.clear();
        bool ok=s.mount_toc(alias,err);
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
        bool duplicated=s.res_entries_total()>before;
        bool same_content=s.res_count()==count;
        err.clear();
        bool missing_rejected=!s.mount_toc(alias+".not_a_real_archive",err);
        std::printf("%s alias read %.3f ms; listings %llu -> %llu; same resources=%d; missing rejected=%d\n",
            legacy?"LITERAL":"NORMALIZED",ms,(unsigned long long)before,
            (unsigned long long)s.res_entries_total(),same_content,missing_rejected);
        if(!ok || !same_content || !missing_rejected || duplicated!=(legacy!=0)) ++failed;
    }
    std::printf("mount_identity_test: %s\n",failed?"FAIL":"PASS");return failed?1:0;
}
