#include "../Arduboy2.h"

Arduboy2Base* arduboy_ptr = nullptr;
Sprites* sprites_ptr = nullptr;

static Sprites ardulib_default_sprites;

Sprites* ardulib_default_sprites_get(void) {
    return &ardulib_default_sprites;
}

void BeepPin1::begin() {
}

void BeepPin1::timer() {
}

uint16_t BeepPin1::freq(uint16_t f) const {
    return f;
}

void BeepPin1::tone(uint16_t, uint16_t) {
}

Arduboy2Base* Sprites::ab_ = nullptr;

void Sprites::setArduboy(Arduboy2Base* a) {
    ab_ = a;
}

void Sprites::drawOverwrite(int16_t x, int16_t y, const uint8_t* bmp, uint8_t frame) {
    if(!ab_ || !bmp) return;
    ab_->drawSolidBitmapFrame(x, y, bmp, frame);
}

void Sprites::drawSelfMasked(int16_t x, int16_t y, const uint8_t* bmp, uint8_t frame) {
    if(!ab_ || !bmp) return;
    ab_->drawBitmapFrame(x, y, bmp, frame);
}

void Sprites::drawErase(int16_t x, int16_t y, const uint8_t* bmp, uint8_t frame) {
    if(!ab_ || !bmp) return;
    ab_->eraseBitmapFrame(x, y, bmp, frame);
}

void Sprites::drawPlusMask(int16_t x, int16_t y, const uint8_t* plusmask, uint8_t frame) {
    if(!ab_) return;
    ab_->drawPlusMask(x, y, plusmask, frame);
}
