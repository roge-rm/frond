#include "DeviceControls.h"
#ifdef DANA_ADAPTIVE_VIEW
#include "DanaView.h"
#endif
#include <cmath>
#include <iostream>
int main() {
#ifdef DANA_ADAPTIVE_VIEW
    DanaViewState state;state.keyboard="compact";state.zoom=2;
    const auto layout=DanaViewLayout::Make(390,760,state);
    const auto width=layout.screen.w;
#else
    const auto layout=danaLayout(390,760,true);
    const auto width=layout.screen.w;
#endif
    if(std::abs(width-1120)>0.01) {
        std::cerr<<"2x guest width changed to "<<width<<" when the 390-wide keyboard was shown\n";
        return 1;
    }
    std::cout<<"2x guest width stays 1120 above a separate 390-wide keyboard\n";
}
