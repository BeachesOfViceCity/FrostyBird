#include "../guest/sbt_prefix.hpp"
#include <array>
#include <cstdio>
int main(){
    alignas(64) std::array<unsigned char,4224> bytes{};unsigned checks=0;
    for(unsigned offset:{0u,1u,8u,16u})for(unsigned pos=0;pos<=128;++pos)for(unsigned word:{0u,8u,16u,24u}){
        auto p=bytes.data()+offset;bytes.fill(0);std::memcpy(p+32*pos+word,&sbt_prefix::marker,8);
        for(unsigned end:{32*pos,32*pos+31,32*pos+32,4096u,4160u}){
            if(sbt_prefix::contains(p,end)!=(word==0 && pos<128 && 32*(pos+1)<=end))return 1;
            ++checks;
        }
    }
    std::printf("SBT prefix: %u aligned/unaligned marker, field and boundary cases matched\n",checks);
}
