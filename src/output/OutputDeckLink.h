#pragma once

#include "Output.h"

#include <memory>

// Blackmagic DeckLink (SDI/HDMI) output backend. Compiled only when
// JPLAY_ENABLE_DECKLINK is defined (the DeckLink SDK headers were found at
// configure time). The SDK's dispatch translation unit dlopen's the driver on
// first use, so the app still launches on machines with no card or no Desktop
// Video install — deckLinkRuntimeAvailable() reports false there and the
// backend can't be selected.

// True if the driver loaded and at least one output-capable device is present.
bool deckLinkRuntimeAvailable();

// A fresh, unopened device bound to the first output-capable card.
std::unique_ptr<OutputDevice> createDeckLinkOutput();
