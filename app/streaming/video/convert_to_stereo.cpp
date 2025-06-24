#include "convert_to_stereo.h"

void ConverToStereo::convertToStereo(Pacer* m_Pacer, AVFrame* frame)
{
    m_Pacer->submitFrame(frame);
}