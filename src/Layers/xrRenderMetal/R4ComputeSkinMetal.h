#pragma once

#import <Metal/Metal.h>

namespace xray::metal
{
// Shared by the independent oracle and the production native pipeline. The
// canonical MSL separately fixes transform/dot FMA and weighted-blend rounding.
inline MTLCompileOptions* R4ComputeSkinCompileOptions(bool diagnosticTrace = false)
{
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.preprocessorMacros = @{@"R4_SKIN_DIAGNOSTIC_TRACE": @(diagnosticTrace)};
    if (@available(macOS 15.0, *))
    {
        options.mathMode = MTLMathModeSafe;
        options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
    }
    else
        options.fastMathEnabled = NO;
    return options;
}
} // namespace xray::metal
