// SPDX-License-Identifier: GPL-3.0-only
#include "ResidentPageValidation.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
namespace {
    using FO4CS::ResidentPages::Check;
    using FO4CS::ResidentPages::Result;
    void Require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
    bool Reference(std::uintptr_t address,std::size_t size) {
        if(!address || !size || size>(std::numeric_limits<std::uintptr_t>::max)()-address) return false;
        const auto end=address+size;
        while(address<end) {
            MEMORY_BASIC_INFORMATION info{};
            if(VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))!=sizeof(info) ||
                info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
            switch(info.Protect&0xffu) {
            case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
            case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: break;
            default:return false;
            }
            address=reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
        }
        return true;
    }
    void Compare(std::uintptr_t address,std::size_t size) {
        const auto result=Check(address,size);
        const bool actual=result==Result::Unknown?Reference(address,size):result==Result::Readable;
        Require(actual==Reference(address,size),"Resident-page result disagrees with current VirtualQuery protections");
    }
}
int main() {
    try {
        const auto page=FO4CS::ResidentPages::PageSize();
        auto* allocation=static_cast<unsigned char*>(VirtualAlloc(nullptr,page*4,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        Require(allocation!=nullptr,"Allocation failed");
        const auto base=reinterpret_cast<std::uintptr_t>(allocation);
        Compare(base,page*4); // committed but not yet resident: must use fallback
        std::memset(allocation,0x55,page*4);
        Require(Check(base,page*4)==Result::Readable,"Resident readable span must use fast proof");
        Require(VirtualLock(allocation,page)!=FALSE,"Small-page lock failed");
        Require(Check(base,1)==Result::Unknown,"Locked mappings must retain allocation-state validation");
        Compare(base,page);
        Require(VirtualUnlock(allocation,page)!=FALSE,"Small-page unlock failed");
        for(const DWORD protection:{PAGE_READONLY,PAGE_READWRITE,PAGE_EXECUTE,PAGE_EXECUTE_READ,PAGE_EXECUTE_READWRITE,PAGE_NOACCESS,PAGE_READWRITE|PAGE_GUARD}) {
            DWORD old{};
            Require(VirtualProtect(allocation+page,page,protection,&old)!=FALSE,"Protection change failed");
            Compare(base+page,1); Compare(base+page-1,2); Compare(base+page,page+1);
            Compare(base,4*page); Compare(base+2*page,2*page);
            MEMORY_BASIC_INFORMATION info{}; VirtualQuery(allocation+page,&info,sizeof(info));
            Require(info.Protect==protection,"Validation must not touch/consume guard pages or change protections");
        }
        DWORD old{}; VirtualProtect(allocation+page,page,PAGE_READWRITE,&old);
        std::memset(allocation+page,1,page);
        Require(Check(base+page,1)==Result::Readable,"Reprotected pages must not retain a stale rejection");
        VirtualFree(allocation+2*page,page,MEM_DECOMMIT);
        Compare(base,4*page); Compare(base+2*page,1);
        Require(VirtualAlloc(allocation+2*page,page,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"Recommit failed");
        Compare(base,4*page);
        VirtualFree(allocation,0,MEM_RELEASE);
        Compare(base,1);
        const auto section=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,page*2,nullptr);
        Require(section!=nullptr,"Mapping creation failed");
        auto* copy=static_cast<unsigned char*>(MapViewOfFile(section,FILE_MAP_COPY,0,0,page*2));
        Require(copy!=nullptr,"Copy-on-write mapping failed");
        Compare(reinterpret_cast<std::uintptr_t>(copy),page*2);
        std::memset(copy,3,page*2);
        Compare(reinterpret_cast<std::uintptr_t>(copy),page*2);
        UnmapViewOfFile(copy); CloseHandle(section);
        Compare(reinterpret_cast<std::uintptr_t>(&Reference),1);
        Compare(0,1); Compare(base,0); Compare((std::numeric_limits<std::uintptr_t>::max)()-1,8);
        Require(Check(base,65ull*page)==Result::Unknown,"Large spans must retain bounded stack and original fallback");
        FO4CS::ResidentPages::enabled=false;
        Require(Check(reinterpret_cast<std::uintptr_t>(&Reference),1)==Result::Unknown,"A/B bypass must retain original validation");
        std::cout<<"PASS: resident/nonresident, all protections, guard preservation, cross-page spans, decommit/recommit/free, copy-on-write, image, overflow and fallback contracts\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
