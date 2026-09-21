#include "../../../Include/xrRender/R4BackendSelection.h"

#include <stdio.h>

namespace
{
bool expect(const char* commandLine, R4BackendKind backend, R4BackendSelectionError error)
{
    const R4BackendSelectionResult result = ParseR4BackendSelection(commandLine);
    if (result.backend != backend || result.error != error)
    {
        fprintf(stderr, "R4_BACKEND_SELECTION_ERROR command_line=\"%s\"\n", commandLine);
        return false;
    }
    return true;
}
}

int main()
{
    bool passed = true;
    passed &= expect("", R4BackendKind::Dxvk, R4BackendSelectionError::None);
    passed &= expect("-load gamma-world", R4BackendKind::Dxvk, R4BackendSelectionError::None);
    passed &= expect("-render_backend dxvk", R4BackendKind::Dxvk, R4BackendSelectionError::None);
    passed &= expect("-render_backend metal", R4BackendKind::Metal, R4BackendSelectionError::None);
    passed &= expect("-render_backend", R4BackendKind::Dxvk,
        R4BackendSelectionError::MissingValue);
    passed &= expect("-render_backend vulkan", R4BackendKind::Dxvk,
        R4BackendSelectionError::UnknownValue);
    passed &= expect("-render_backend=metal", R4BackendKind::Dxvk,
        R4BackendSelectionError::UnknownValue);
    passed &= expect("-render_backend metal -render_backend metal", R4BackendKind::Metal,
        R4BackendSelectionError::DuplicateOption);

    if (!passed)
        return 1;

    SetSelectedR4Backend(R4BackendKind::Metal);
    if (SelectedR4Backend() != R4BackendKind::Metal ||
        std::string_view(R4BackendName(SelectedR4Backend())) != "metal")
        return 1;

    printf("R4_BACKEND_SELECTION_READY default=dxvk opt_in=metal invalid_values=rejected\n");
    return 0;
}
