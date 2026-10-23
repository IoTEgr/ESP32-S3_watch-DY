/* Generated from tools/watchface/layout.json. */
#pragma once
#define FACE_WIDTH 410
#define FACE_HEIGHT 502
#define FACE_COLOR_BACKGROUND 0xF3EFF9
#define FACE_COLOR_TIME 0x586FAC
#define FACE_COLOR_META 0x687EAB
#define FACE_COLOR_PHRASE 0xA6A1C5
#define FACE_COLOR_TRACK 0xDEE5F3
#define FACE_COLOR_FILL_A 0xA6C8ED
#define FACE_COLOR_FILL_B 0x86B6E6
#define FACE_TIME_X -3
#define FACE_TIME_Y 70
#define FACE_DATE_X 111
#define FACE_DATE_Y 44
#define FACE_DATE_LETTER_SPACE 2
#define FACE_WEEKDAY_RIGHT 286
#define FACE_WEEKDAY_Y 44
#define FACE_BATTERY_X 99
#define FACE_BATTERY_Y 156
#define FACE_BATTERY_W 99
#define FACE_BATTERY_H 14
#define FACE_WEATHER_X 208
#define FACE_WEATHER_Y 140
#define FACE_WEATHER_W 50
#define FACE_WEATHER_H 38
#define FACE_TEMPERATURE_X 279
#define FACE_TEMPERATURE_Y 151
#define FACE_ANIMATION_CENTER_X 205
#define FACE_ANIMATION_CENTER_Y 312
#define FACE_ANIMATION_SCALE 277
typedef struct {const char *text; int16_t x,y; uint8_t right,size;} face_phrase_t;
typedef struct {face_phrase_t phrases[8];} face_scene_t;
static const face_scene_t FACE_SCENES[4] = {
    {{
        {"笨蛋",44,205,0,1},
        {"对不起我对了",383,184,1,0},
        {"哼",36,320,0,2},
        {"^-w-^",370,316,1,0},
        {"略略略",44,409,0,1},
        {"大事很妙",383,410,1,1},
    }},
    {{
        {"^-τ-マ",18,205,0,1},
        {"Look at 咪",388,182,1,1},
        {"咪瑜伽",9,282,0,0},
        {"萌",16,352,0,1},
        {"喵",384,340,1,1},
        {"rPr~",385,405,1,2},
    }},
    {{
        {"稳住！",24,216,0,1},
        {"还在硬撑",385,197,1,0},
        {"汗",13,310,0,2},
        {"？",353,265,1,2},
        {"^-w-^",373,334,1,0},
        {"尬",25,419,0,1},
        {"莫慌",384,406,1,1},
    }},
    {{
        {"为什么",11,207,0,1},
        {"我真的特别爱你",363,191,1,1},
        {"喵",398,270,1,2},
        {"你会落泪",8,406,0,2},
        {"-w-",373,405,1,1},
    }},
};
