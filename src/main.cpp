/*
    SPDX-FileCopyrightText: 2026 Trail KWin Effect contributors
    SPDX-License-Identifier: GPL-3.0-only
*/

#include "traileffect.h"

#include <kwin/effect/effecthandler.h>

namespace KWin
{

// The effect only makes sense with a hardware accelerated compositor; the GL
// rendering path is the only one implemented.
KWIN_EFFECT_FACTORY_SUPPORTED(TrailEffect,
                              "metadata.json",
                              return effects->isOpenGLCompositing();)

} // namespace KWin

#include "main.moc"
