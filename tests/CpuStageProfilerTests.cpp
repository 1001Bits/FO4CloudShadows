// SPDX-License-Identifier: GPL-3.0-only
#include "CpuStageProfiler.h"
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace FO4CS::CpuProfile;
namespace {
    void Require(bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
    }
    const Counter& At(Stage stage) { return counters[static_cast<unsigned>(stage)]; }
}
int main() {
    try {
        { Scope outer(Stage::CaptureTotal); Scope inner(Stage::CaptureDraw); }
        Require(At(Stage::CaptureDraw).calls == 0 && current == nullptr,"Disabled scopes must not record or alter nesting");
        enabled=true;
        {
            Scope outer(Stage::CaptureTotal);
            { Scope child(Stage::CaptureDraw); }
            { Scope child(Stage::CaptureSave); child.Set(Stage::CaptureBind); child.Stop(); child.Stop(); }
        }
        const auto parent=At(Stage::CaptureTotal);
        const auto children=At(Stage::CaptureDraw).ticks+At(Stage::CaptureSave).ticks+At(Stage::CaptureBind).ticks;
        Require(parent.calls==1 && parent.ticks==parent.exclusiveTicks+children,"Nested durations must partition exactly without double counting");
        Require(At(Stage::CaptureSave).calls==1 && At(Stage::CaptureBind).calls==1,"Switching stages and stopping twice must not double count");
        Require(current==nullptr,"Scopes must restore the parent stack");
        {
            Scope outer(Stage::MemoryValidation);
            { Scope child(Stage::MemoryValidation); }
        }
        Require(At(Stage::MemoryValidation).calls==2 && At(Stage::MemoryValidation).exclusiveTicks<=At(Stage::MemoryValidation).ticks,"Recursive stage labels must preserve separate inclusive and exclusive totals");
        std::thread worker([] {
            Require(At(Stage::CaptureTotal).calls==0,"Worker counters must start empty");
            Scope work(Stage::CaptureTotal);
        });
        worker.join();
        Require(At(Stage::CaptureTotal).calls==1,"Worker counters must not contaminate the Present thread");
        measureCycles=true;
        { Scope cycle(Stage::Present); for (unsigned i=0;i<100;++i) (void)Now(); }
        Require(At(Stage::Present).cycles>0,"Optional thread-cycle measurement must record actual scheduled cycles");
        enabled=false;
        std::cout << "PASS: disabled, nested, switched, recursive, thread-local and optional-cycle stage timing contracts\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
