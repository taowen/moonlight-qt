#pragma once
#include "ffmpeg-renderers/pacer/pacer.h"

namespace ConverToStereo {
    void convertToStereo(Pacer* m_Pacer, AVFrame* frame);
}