#pragma once

// Convert SDL window coordinates back into the panel-native normalized frame
// expected by the firmware's GfxRenderer::tapToLogical().
void simulatorWindowPointToPanelNormalized(int windowX, int windowY, float &nx,
                                           float &ny);
