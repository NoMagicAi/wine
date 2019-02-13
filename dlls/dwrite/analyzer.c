/*
 *    Text analyzer
 *
 * Copyright 2011 Aric Stewart for CodeWeavers
 * Copyright 2012, 2014 Nikolay Sivov for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <math.h>

#include "dwrite_private.h"
#include "scripts.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwrite);

extern const unsigned short wine_linebreak_table[] DECLSPEC_HIDDEN;
extern const unsigned short wine_scripts_table[] DECLSPEC_HIDDEN;

/* Number of characters needed for LOCALE_SNATIVEDIGITS */
#define NATIVE_DIGITS_LEN 11

struct dwritescript_properties {
    DWRITE_SCRIPT_PROPERTIES props;
    UINT32 scripttags[3]; /* Maximum 2 script tags, 0-terminated. */
    BOOL is_complex;
    const struct scriptshaping_ops *ops;
};

#define _OT(a,b,c,d) DWRITE_MAKE_OPENTYPE_TAG(a,b,c,d)

/* NOTE: keep this array synced with script ids from scripts.h */
static const struct dwritescript_properties dwritescripts_properties[Script_LastId+1] = {
    { /* Zzzz */ { 0x7a7a7a5a, 999, 15, 0x0020, 0, 0, 0, 0, 0, 0, 0 } },
    { /* Zyyy */ { 0x7979795a, 998,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 } },
    { /* Zinh */ { 0x686e695a, 994, 15, 0x0020, 1, 0, 0, 0, 0, 0, 0 } },
    { /* Arab */ { 0x62617241, 160,  8, 0x0640, 0, 1, 0, 0, 0, 1, 1 }, { _OT('a','r','a','b') }, TRUE },
    { /* Armn */ { 0x6e6d7241, 230,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('a','r','m','n') } },
    { /* Avst */ { 0x74737641, 134,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('a','v','s','t') } },
    { /* Bali */ { 0x696c6142, 360, 15, 0x0020, 1, 0, 1, 0, 0, 0, 0 }, { _OT('b','a','l','i') } },
    { /* Bamu */ { 0x756d6142, 435,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('b','a','m','u') } },
    { /* Batk */ { 0x6b746142, 365,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('b','a','t','k') } },
    { /* Beng */ { 0x676e6542, 325, 15, 0x0020, 1, 1, 0, 0, 0, 1, 0 }, { _OT('b','n','g','2'), _OT('b','e','n','g') }, TRUE },
    { /* Bopo */ { 0x6f706f42, 285,  1, 0x0020, 0, 0, 1, 1, 0, 0, 0 }, { _OT('b','o','p','o') } },
    { /* Brah */ { 0x68617242, 300,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('b','r','a','h') } },
    { /* Brai */ { 0x69617242, 570,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('b','r','a','i') }, TRUE },
    { /* Bugi */ { 0x69677542, 367,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('b','u','g','i') } },
    { /* Buhd */ { 0x64687542, 372,  8, 0x0020, 0, 0, 1, 0, 0, 0, 0 }, { _OT('b','u','h','d') } },
    { /* Cans */ { 0x736e6143, 440,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('c','a','n','s') }, TRUE },
    { /* Cari */ { 0x69726143, 201,  1, 0x0020, 0, 0, 1, 0, 0, 0, 0 }, { _OT('c','a','r','i') } },
    { /* Cham */ { 0x6d616843, 358,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('c','h','a','m') } },
    { /* Cher */ { 0x72656843, 445,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('c','h','e','r') }, TRUE },
    { /* Copt */ { 0x74706f43, 204,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('c','o','p','t') } },
    { /* Xsux */ { 0x78757358,  20,  1, 0x0020, 0, 0, 1, 1, 0, 0, 0 }, { _OT('x','s','u','x') } },
    { /* Cprt */ { 0x74727043, 403,  1, 0x0020, 0, 0, 1, 0, 0, 0, 0 }, { _OT('c','p','r','t') } },
    { /* Cyrl */ { 0x6c727943, 220,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('c','y','r','l') } },
    { /* Dsrt */ { 0x74727344, 250,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('d','s','r','t') }, TRUE },
    { /* Deva */ { 0x61766544, 315, 15, 0x0020, 1, 1, 0, 0, 0, 1, 0 }, { _OT('d','e','v','2'), _OT('d','e','v','a') }, TRUE },
    { /* Egyp */ { 0x70796745,  50,  1, 0x0020, 0, 0, 1, 1, 0, 0, 0 }, { _OT('e','g','y','p') } },
    { /* Ethi */ { 0x69687445, 430,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('e','t','h','i') }, TRUE },
    { /* Geor */ { 0x726f6547, 240,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('g','e','o','r') } },
    { /* Glag */ { 0x67616c47, 225,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('g','l','a','g') } },
    { /* Goth */ { 0x68746f47, 206,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('g','o','t','h') } },
    { /* Grek */ { 0x6b657247, 200,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('g','r','e','k') } },
    { /* Gujr */ { 0x726a7547, 320, 15, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('g','j','r','2'), _OT('g','u','j','r') }, TRUE },
    { /* Guru */ { 0x75727547, 310, 15, 0x0020, 1, 1, 0, 0, 0, 1, 0 }, { _OT('g','u','r','2'), _OT('g','u','r','u') }, TRUE },
    { /* Hani */ { 0x696e6148, 500,  8, 0x0020, 0, 0, 1, 1, 0, 0, 0 }, { _OT('h','a','n','i') } },
    { /* Hang */ { 0x676e6148, 286,  8, 0x0020, 1, 1, 1, 1, 0, 0, 0 }, { _OT('h','a','n','g') }, TRUE },
    { /* Hano */ { 0x6f6e6148, 371,  8, 0x0020, 0, 0, 1, 0, 0, 0, 0 }, { _OT('h','a','n','o') } },
    { /* Hebr */ { 0x72626548, 125,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('h','e','b','r') }, TRUE },
    { /* Hira */ { 0x61726948, 410,  8, 0x0020, 0, 0, 1, 1, 0, 0, 0 }, { _OT('k','a','n','a') } },
    { /* Armi */ { 0x696d7241, 124,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('a','r','m','i') } },
    { /* Phli */ { 0x696c6850, 131,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('p','h','l','i') } },
    { /* Prti */ { 0x69747250, 130,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('p','r','t','i') } },
    { /* Java */ { 0x6176614a, 361, 15, 0x0020, 1, 0, 1, 0, 0, 0, 0 }, { _OT('j','a','v','a') } },
    { /* Kthi */ { 0x6968744b, 317, 15, 0x0020, 1, 1, 0, 0, 0, 1, 0 }, { _OT('k','t','h','i') } },
    { /* Knda */ { 0x61646e4b, 345, 15, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('k','n','d','2'), _OT('k','n','d','a') }, TRUE },
    { /* Kana */ { 0x616e614b, 411,  8, 0x0020, 0, 0, 1, 1, 0, 0, 0 }, { _OT('k','a','n','a') } },
    { /* Kali */ { 0x696c614b, 357,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('k','a','l','i') } },
    { /* Khar */ { 0x7261684b, 305, 15, 0x0020, 1, 0, 1, 0, 0, 0, 0 }, { _OT('k','h','a','r') } },
    { /* Khmr */ { 0x726d684b, 355,  8, 0x0020, 1, 0, 1, 0, 1, 0, 0 }, { _OT('k','h','m','r') }, TRUE },
    { /* Laoo */ { 0x6f6f614c, 356,  8, 0x0020, 1, 0, 1, 0, 1, 0, 0 }, { _OT('l','a','o',' ') }, TRUE },
    { /* Latn */ { 0x6e74614c, 215,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('l','a','t','n') }, FALSE, &latn_shaping_ops },
    { /* Lepc */ { 0x6370654c, 335,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('l','e','p','c') } },
    { /* Limb */ { 0x626d694c, 336,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('l','i','m','b') } },
    { /* Linb */ { 0x626e694c, 401,  1, 0x0020, 0, 0, 1, 1, 0, 0, 0 }, { _OT('l','i','n','b') } },
    { /* Lisu */ { 0x7573694c, 399,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('l','i','s','u') } },
    { /* Lyci */ { 0x6963794c, 202,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('l','y','c','i') } },
    { /* Lydi */ { 0x6964794c, 116,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('l','y','d','i') } },
    { /* Mlym */ { 0x6d796c4d, 347, 15, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('m','l','m','2'), _OT('m','l','y','m') }, TRUE },
    { /* Mand */ { 0x646e614d, 140,  8, 0x0640, 0, 1, 0, 0, 0, 1, 1 }, { _OT('m','a','n','d') } },
    { /* Mtei */ { 0x6965744d, 337,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('m','t','e','i') } },
    { /* Mong */ { 0x676e6f4d, 145,  8, 0x0020, 0, 1, 0, 0, 0, 1, 1 }, { _OT('m','o','n','g') }, TRUE },
    { /* Mymr */ { 0x726d794d, 350, 15, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('m','y','m','r') }, TRUE },
    { /* Talu */ { 0x756c6154, 354,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('t','a','l','u') }, TRUE },
    { /* Nkoo */ { 0x6f6f6b4e, 165,  8, 0x0020, 0, 1, 0, 0, 0, 1, 1 }, { _OT('n','k','o',' ') }, TRUE },
    { /* Ogam */ { 0x6d61674f, 212,  1, 0x1680, 0, 1, 0, 0, 0, 1, 0 }, { _OT('o','g','a','m') }, TRUE },
    { /* Olck */ { 0x6b636c4f, 261,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('o','l','c','k') } },
    { /* Ital */ { 0x6c617449, 210,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('i','t','a','l') } },
    { /* Xpeo */ { 0x6f657058,  30,  1, 0x0020, 0, 1, 1, 1, 0, 0, 0 }, { _OT('x','p','e','o') }, TRUE },
    { /* Sarb */ { 0x62726153, 105,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('s','a','r','b') } },
    { /* Orkh */ { 0x686b724f, 175,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('o','r','k','h') } },
    { /* Orya */ { 0x6179724f, 327, 15, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('o','r','y','2'), _OT('o','r','y','a') }, TRUE },
    { /* Osma */ { 0x616d734f, 260,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('o','s','m','a') }, TRUE },
    { /* Phag */ { 0x67616850, 331,  8, 0x0020, 0, 1, 0, 0, 0, 1, 1 }, { _OT('p','h','a','g') }, TRUE },
    { /* Phnx */ { 0x786e6850, 115,  1, 0x0020, 0, 0, 1, 0, 0, 0, 0 }, { _OT('p','h','n','x') } },
    { /* Rjng */ { 0x676e6a52, 363,  8, 0x0020, 1, 0, 1, 0, 0, 0, 0 }, { _OT('r','j','n','g') } },
    { /* Runr */ { 0x726e7552, 211,  1, 0x0020, 0, 0, 1, 0, 0, 0, 0 }, { _OT('r','u','n','r') }, TRUE },
    { /* Samr */ { 0x726d6153, 123,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('s','a','m','r') } },
    { /* Saur */ { 0x72756153, 344,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('s','a','u','r') } },
    { /* Shaw */ { 0x77616853, 281,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('s','h','a','w') } },
    { /* Sinh */ { 0x686e6953, 348,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('s','i','n','h') }, TRUE },
    { /* Sund */ { 0x646e7553, 362,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('s','u','n','d') } },
    { /* Sylo */ { 0x6f6c7953, 316,  8, 0x0020, 1, 1, 0, 0, 0, 1, 0 }, { _OT('s','y','l','o') } },
    { /* Syrc */ { 0x63727953, 135,  8, 0x0640, 0, 1, 0, 0, 0, 1, 1 }, { _OT('s','y','r','c') }, TRUE },
    { /* Tglg */ { 0x676c6754, 370,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('t','g','l','g') } },
    { /* Tagb */ { 0x62676154, 373,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('t','a','g','b') } },
    { /* Tale */ { 0x656c6154, 353,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('t','a','l','e') }, TRUE },
    { /* Lana */ { 0x616e614c, 351,  8, 0x0020, 1, 0, 1, 0, 0, 0, 0 }, { _OT('l','a','n','a') } },
    { /* Tavt */ { 0x74766154, 359,  8, 0x0020, 1, 0, 1, 0, 1, 0, 0 }, { _OT('t','a','v','t') } },
    { /* Taml */ { 0x6c6d6154, 346, 15, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('t','m','l','2'), _OT('t','a','m','l') }, TRUE },
    { /* Telu */ { 0x756c6554, 340, 15, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('t','e','l','2'), _OT('t','e','l','u') }, TRUE },
    { /* Thaa */ { 0x61616854, 170,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('t','h','a','a') }, TRUE },
    { /* Thai */ { 0x69616854, 352,  8, 0x0020, 1, 0, 1, 0, 1, 0, 0 }, { _OT('t','h','a','i') }, TRUE },
    { /* Tibt */ { 0x74626954, 330,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('t','i','b','t') }, TRUE },
    { /* Tfng */ { 0x676e6654, 120,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('t','f','n','g') }, TRUE },
    { /* Ugar */ { 0x72616755,  40,  1, 0x0020, 0, 0, 1, 1, 0, 0, 0 }, { _OT('u','g','a','r') } },
    { /* Vaii */ { 0x69696156, 470,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('v','a','i',' ') }, TRUE },
    { /* Yiii */ { 0x69696959, 460,  1, 0x0020, 0, 0, 1, 1, 0, 0, 0 }, { _OT('y','i',' ',' ') }, TRUE },
    { /* Cakm */ { 0x6d6b6143, 349,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('c','a','k','m') } },
    { /* Merc */ { 0x6372654d, 101,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('m','e','r','c') } },
    { /* Mero */ { 0x6f72654d, 100,  1, 0x0020, 0, 1, 1, 1, 0, 0, 0 }, { _OT('m','e','r','o') } },
    { /* Plrd */ { 0x64726c50, 282,  8, 0x0020, 1, 0, 1, 0, 0, 0, 0 }, { _OT('p','l','r','d') } },
    { /* Shrd */ { 0x64726853, 319,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('s','h','r','d') } },
    { /* Sora */ { 0x61726f53, 398,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('s','o','r','a') } },
    { /* Takr */ { 0x726b6154, 321,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('t','a','k','r') } },
    { /* Bass */ { 0x73736142, 259,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('b','a','s','s') } },
    { /* Aghb */ { 0x62686741, 239,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('a','g','h','b') } },
    { /* Dupl */ { 0x6c707544, 755,  8, 0x0020, 0, 1, 0, 0, 0, 1, 1 }, { _OT('d','u','p','l') } },
    { /* Elba */ { 0x61626c45, 226,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('e','l','b','a') } },
    { /* Gran */ { 0x6e617247, 343, 15, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('g','r','a','n') } },
    { /* Khoj */ { 0x6a6f684b, 322, 15, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('k','h','o','j') } },
    { /* Sind */ { 0x646e6953, 318,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('s','i','n','d') } },
    { /* Lina */ { 0x616e694c, 400,  1, 0x0020, 0, 0, 1, 1, 0, 0, 0 }, { _OT('l','i','n','a') } },
    { /* Mahj */ { 0x6a68614d, 314,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('m','a','h','j') } },
    { /* Mani */ { 0x696e614d, 139,  8, 0x0640, 0, 1, 0, 0, 0, 1, 1 }, { _OT('m','a','n','i') } },
    { /* Mend */ { 0x646e654d, 438,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('m','e','n','d') } },
    { /* Modi */ { 0x69646f4d, 324, 15, 0x0020, 1, 1, 0, 0, 0, 1, 0 }, { _OT('m','o','d','i') } },
    { /* Mroo */ { 0x6f6f724d, 199,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('m','r','o','o') } },
    { /* Nbat */ { 0x7461624e, 159,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('n','b','a','t') } },
    { /* Narb */ { 0x6272614e, 106,  1, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('n','a','r','b') } },
    { /* Perm */ { 0x6d726550, 227,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('p','e','r','m') } },
    { /* Hmng */ { 0x676e6d48, 450,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('h','m','n','g') } },
    { /* Palm */ { 0x6d6c6150, 126,  8, 0x0020, 1, 1, 1, 0, 0, 0, 0 }, { _OT('p','a','l','m') } },
    { /* Pauc */ { 0x63756150, 263,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('p','a','u','c') } },
    { /* Phlp */ { 0x706c6850, 132,  8, 0x0640, 0, 1, 0, 0, 0, 1, 1 }, { _OT('p','h','l','p') } },
    { /* Sidd */ { 0x64646953, 302,  8, 0x0020, 1, 0, 1, 1, 0, 0, 0 }, { _OT('s','i','d','d') } },
    { /* Tirh */ { 0x68726954, 326, 15, 0x0020, 1, 1, 0, 0, 0, 1, 0 }, { _OT('t','i','r','h') } },
    { /* Wara */ { 0x61726157, 262,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('w','a','r','a') } },
    { /* Adlm */ { 0x6d6c6441, 166,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('a','d','l','m') } },
    { /* Ahom */ { 0x6d6f6841, 338,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('a','h','o','m') } },
    { /* Hluw */ { 0x77756c48,  80,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('h','l','u','w') } },
    { /* Bhks */ { 0x736b6842, 334,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('b','h','k','s') } },
    { /* Hatr */ { 0x72746148, 127,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('h','a','t','r') } },
    { /* Marc */ { 0x6372614d, 332,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('m','a','r','c') } },
    { /* Mult */ { 0x746c754d, 323,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('m','u','l','t') } },
    { /* Newa */ { 0x6177654e, 333,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('n','e','w','a') } },
    { /* Hung */ { 0x676e7548, 176,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('h','u','n','g') } },
    { /* Osge */ { 0x6567734f, 219,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('o','s','g','e') } },
    { /* Sgnw */ { 0x776e6753,  95,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('s','g','n','w') } },
    { /* Tang */ { 0x676e6154, 520,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('t','a','n','g') } },
    { /* Gonm */ { 0x6d6e6f47, 313,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('g','o','n','m') } },
    { /* Nshu */ { 0x7568734e, 499,  1, 0x0020, 0, 0, 1, 1, 0, 0, 0 }, { _OT('n','s','h','u') } },
    { /* Soyo */ { 0x6f796f53, 329,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('s','o','y','o') } },
    { /* Zanb */ { 0x626e615a, 339,  8, 0x0020, 0, 1, 1, 0, 0, 0, 0 }, { _OT('z','a','n','b') } },
};
#undef _OT

const char *debugstr_sa_script(UINT16 script)
{
    return script < Script_LastId ? debugstr_tag(dwritescripts_properties[script].props.isoScriptCode) : "undefined";
}

/* system font falback configuration */
static const WCHAR meiryoW[] = {'M','e','i','r','y','o',0};

static const WCHAR *cjk_families[] = { meiryoW };

static const DWRITE_UNICODE_RANGE cjk_ranges[] =
{
    { 0x3000, 0x30ff }, /* CJK Symbols and Punctuation, Hiragana, Katakana */
    { 0x31f0, 0x31ff }, /* Katakana Phonetic Extensions */
    { 0x4e00, 0x9fff }, /* CJK Unified Ideographs */
};

struct fallback_mapping {
    DWRITE_UNICODE_RANGE *ranges;
    UINT32 ranges_count;
    WCHAR **families;
    UINT32 families_count;
    IDWriteFontCollection *collection;
    WCHAR *locale;
    FLOAT scale;
};

static const struct fallback_mapping fontfallback_neutral_data[] = {
#define MAPPING_RANGE(ranges, families) \
        { (DWRITE_UNICODE_RANGE *)ranges, ARRAY_SIZE(ranges), \
          (WCHAR **)families, ARRAY_SIZE(families) }

    MAPPING_RANGE(cjk_ranges, cjk_families),

#undef MAPPING_RANGE
};

struct dwrite_fontfallback {
    IDWriteFontFallback IDWriteFontFallback_iface;
    LONG ref;
    IDWriteFactory5 *factory;
    IDWriteFontCollection1 *systemcollection;
    struct fallback_mapping *mappings;
    UINT32 mappings_count;
};

struct dwrite_fontfallback_builder {
    IDWriteFontFallbackBuilder IDWriteFontFallbackBuilder_iface;
    LONG ref;
    IDWriteFactory5 *factory;
    struct fallback_mapping *mappings;
    UINT32 mappings_count;
    UINT32 mappings_capacity;
};

struct dwrite_numbersubstitution {
    IDWriteNumberSubstitution IDWriteNumberSubstitution_iface;
    LONG ref;

    DWRITE_NUMBER_SUBSTITUTION_METHOD method;
    WCHAR *locale;
    BOOL ignore_user_override;
};

static inline struct dwrite_numbersubstitution *impl_from_IDWriteNumberSubstitution(IDWriteNumberSubstitution *iface)
{
    return CONTAINING_RECORD(iface, struct dwrite_numbersubstitution, IDWriteNumberSubstitution_iface);
}

static struct dwrite_numbersubstitution *unsafe_impl_from_IDWriteNumberSubstitution(IDWriteNumberSubstitution *iface);

static inline struct dwrite_fontfallback *impl_from_IDWriteFontFallback(IDWriteFontFallback *iface)
{
    return CONTAINING_RECORD(iface, struct dwrite_fontfallback, IDWriteFontFallback_iface);
}

static inline struct dwrite_fontfallback_builder *impl_from_IDWriteFontFallbackBuilder(IDWriteFontFallbackBuilder *iface)
{
    return CONTAINING_RECORD(iface, struct dwrite_fontfallback_builder, IDWriteFontFallbackBuilder_iface);
}

static inline UINT32 decode_surrogate_pair(const WCHAR *str, UINT32 index, UINT32 end)
{
    if (index < end-1 && IS_SURROGATE_PAIR(str[index], str[index+1])) {
        UINT32 ch = 0x10000 + ((str[index] - 0xd800) << 10) + (str[index+1] - 0xdc00);
        TRACE("surrogate pair (%x %x) => %x\n", str[index], str[index+1], ch);
        return ch;
    }
    return 0;
}

static inline UINT16 get_char_script(WCHAR c)
{
    UINT16 script = get_table_entry(wine_scripts_table, c);
    return script == Script_Inherited ? Script_Unknown : script;
}

static DWRITE_SCRIPT_ANALYSIS get_char_sa(WCHAR c)
{
    DWRITE_SCRIPT_ANALYSIS sa;

    sa.script = get_char_script(c);
    sa.shapes = iscntrlW(c) || c == 0x2028 /* LINE SEPARATOR */ || c == 0x2029 /* PARAGRAPH SEPARATOR */ ?
        DWRITE_SCRIPT_SHAPES_NO_VISUAL : DWRITE_SCRIPT_SHAPES_DEFAULT;
    return sa;
}

static HRESULT analyze_script(const WCHAR *text, UINT32 position, UINT32 length, IDWriteTextAnalysisSink *sink)
{
    DWRITE_SCRIPT_ANALYSIS sa;
    UINT32 pos, i, seq_length;

    if (!length)
        return S_OK;

    sa = get_char_sa(*text);

    pos = position;
    seq_length = 1;

    for (i = 1; i < length; i++)
    {
        DWRITE_SCRIPT_ANALYSIS cur_sa = get_char_sa(text[i]);

        /* Unknown type is ignored when preceded or followed by another script */
        switch (sa.script) {
        case Script_Unknown:
            sa.script = cur_sa.script;
            break;
        case Script_Common:
            if (cur_sa.script == Script_Unknown)
                cur_sa.script = sa.script;
            else if ((cur_sa.script != Script_Common) && sa.shapes == DWRITE_SCRIPT_SHAPES_DEFAULT)
                sa.script = cur_sa.script;
            break;
        default:
            if ((cur_sa.script == Script_Common && cur_sa.shapes == DWRITE_SCRIPT_SHAPES_DEFAULT) || cur_sa.script == Script_Unknown)
                cur_sa.script = sa.script;
        }

        /* this is a length of a sequence to be reported next */
        if (sa.script == cur_sa.script && sa.shapes == cur_sa.shapes)
            seq_length++;
        else {
            HRESULT hr;

            hr = IDWriteTextAnalysisSink_SetScriptAnalysis(sink, pos, seq_length, &sa);
            if (FAILED(hr)) return hr;
            pos = position + i;
            seq_length = 1;
            sa = cur_sa;
        }
    }

    /* one char length case or normal completion call */
    return IDWriteTextAnalysisSink_SetScriptAnalysis(sink, pos, seq_length, &sa);
}

struct linebreaking_state {
    DWRITE_LINE_BREAKPOINT *breakpoints;
    UINT32 count;
};

enum BreakConditionLocation {
    BreakConditionBefore,
    BreakConditionAfter
};

enum linebreaking_classes {
    b_BK = 1,
    b_CR,
    b_LF,
    b_CM,
    b_SG,
    b_GL,
    b_CB,
    b_SP,
    b_ZW,
    b_NL,
    b_WJ,
    b_JL,
    b_JV,
    b_JT,
    b_H2,
    b_H3,
    b_XX,
    b_OP,
    b_CL,
    b_CP,
    b_QU,
    b_NS,
    b_EX,
    b_SY,
    b_IS,
    b_PR,
    b_PO,
    b_NU,
    b_AL,
    b_ID,
    b_IN,
    b_HY,
    b_BB,
    b_BA,
    b_SA,
    b_AI,
    b_B2,
    b_HL,
    b_CJ,
    b_RI,
    b_EB,
    b_EM,
    b_ZWJ,
};

static BOOL has_strong_condition(DWRITE_BREAK_CONDITION old_condition, DWRITE_BREAK_CONDITION new_condition)
{
    if (old_condition == DWRITE_BREAK_CONDITION_MAY_NOT_BREAK || old_condition == DWRITE_BREAK_CONDITION_MUST_BREAK)
        return TRUE;

    if (old_condition == DWRITE_BREAK_CONDITION_CAN_BREAK && new_condition != DWRITE_BREAK_CONDITION_MUST_BREAK)
        return TRUE;

    return FALSE;
}

/* "Can break" is a weak condition, stronger "may not break" and "must break" override it. Initially all conditions are
    set to "can break" and could only be changed once. */
static inline void set_break_condition(UINT32 pos, enum BreakConditionLocation location, DWRITE_BREAK_CONDITION condition,
    struct linebreaking_state *state)
{
    if (location == BreakConditionBefore) {
        if (has_strong_condition(state->breakpoints[pos].breakConditionBefore, condition))
            return;
        state->breakpoints[pos].breakConditionBefore = condition;
        if (pos > 0)
            state->breakpoints[pos-1].breakConditionAfter = condition;
    }
    else {
        if (has_strong_condition(state->breakpoints[pos].breakConditionAfter, condition))
            return;
        state->breakpoints[pos].breakConditionAfter = condition;
        if (pos + 1 < state->count)
            state->breakpoints[pos+1].breakConditionBefore = condition;
    }
}

BOOL lb_is_newline_char(WCHAR ch)
{
    short c = get_table_entry(wine_linebreak_table, ch);
    return c == b_LF || c == b_NL || c == b_CR || c == b_BK;
}

static HRESULT analyze_linebreaks(const WCHAR *text, UINT32 count, DWRITE_LINE_BREAKPOINT *breakpoints)
{
    struct linebreaking_state state;
    short *break_class;
    int i, j;

    break_class = heap_calloc(count, sizeof(*break_class));
    if (!break_class)
        return E_OUTOFMEMORY;

    state.breakpoints = breakpoints;
    state.count = count;

    for (i = 0; i < count; i++)
    {
        break_class[i] = get_table_entry(wine_linebreak_table, text[i]);

        breakpoints[i].breakConditionBefore = DWRITE_BREAK_CONDITION_NEUTRAL;
        breakpoints[i].breakConditionAfter  = DWRITE_BREAK_CONDITION_NEUTRAL;
        breakpoints[i].isWhitespace = !!isspaceW(text[i]);
        breakpoints[i].isSoftHyphen = text[i] == 0x00ad /* Unicode Soft Hyphen */;
        breakpoints[i].padding = 0;

        /* LB1 - resolve some classes. TODO: use external algorithms for these classes. */
        switch (break_class[i])
        {
            case b_AI:
            case b_SA:
            case b_SG:
            case b_XX:
                break_class[i] = b_AL;
                break;
            case b_CJ:
                break_class[i] = b_NS;
                break;
        }
    }

    /* LB2 - never break at the start */
    set_break_condition(0, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
    /* LB3 - always break at the end. */
    set_break_condition(count - 1, BreakConditionAfter, DWRITE_BREAK_CONDITION_CAN_BREAK, &state);

    /* LB4 - LB6 - mandatory breaks. */
    for (i = 0; i < count; i++)
    {
        switch (break_class[i])
        {
            /* LB4 - LB6 */
            case b_CR:
                /* LB5 - don't break CR x LF */
                if (i < count-1 && break_class[i+1] == b_LF)
                {
                    set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                    set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                    break;
                }
            case b_LF:
            case b_NL:
            case b_BK:
                /* LB4 - LB5 - always break after hard breaks */
                set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MUST_BREAK, &state);
                /* LB6 - do not break before hard breaks */
                set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                break;
        }
    }

    /* LB7 - LB8 - explicit breaks and non-breaks */
    for (i = 0; i < count; i++)
    {
        switch (break_class[i])
        {
            /* LB7 - do not break before spaces or zero-width space */
            case b_SP:
                set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                break;
            case b_ZW:
                set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);

                /* LB8 - break before character after zero-width space, skip spaces in-between */
                j = i;
                while (j < count-1 && break_class[j+1] == b_SP)
                    j++;
                if (j < count-1 && break_class[j+1] != b_ZW)
                    set_break_condition(j, BreakConditionAfter, DWRITE_BREAK_CONDITION_CAN_BREAK, &state);
                break;
            /* LB8a - do not break after ZWJ */
            case b_ZWJ:
                set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                break;
        }
    }

    /* LB9 - LB10 - combining marks */
    for (i = 0; i < count; i++)
    {
        if (break_class[i] == b_CM || break_class[i] == b_ZWJ)
        {
            if (i > 0)
            {
                switch (break_class[i-1])
                {
                    case b_SP:
                    case b_BK:
                    case b_CR:
                    case b_LF:
                    case b_NL:
                    case b_ZW:
                        break_class[i] = b_AL;
                        break;
                    default:
                    {
                        break_class[i] = break_class[i-1];
                        set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                    }
                }
            }
            else break_class[i] = b_AL;
        }
    }

    for (i = 0; i < count; i++)
    {
        switch (break_class[i])
        {
            /* LB11 - don't break before and after word joiner */
            case b_WJ:
                set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                break;
            /* LB12 - don't break after glue */
            case b_GL:
                set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
            /* LB12a */
                if (i > 0)
                {
                    if (break_class[i-1] != b_SP && break_class[i-1] != b_BA && break_class[i-1] != b_HY)
                        set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                }
                break;
            /* LB13 */
            case b_CL:
            case b_CP:
            case b_EX:
            case b_IS:
            case b_SY:
                set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                break;
            /* LB14 - do not break after OP, even after spaces */
            case b_OP:
                j = i;
                while (j < count-1 && break_class[j+1] == b_SP)
                    j++;
                set_break_condition(j, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                break;
            /* LB15 - do not break within QU-OP, even with intervening spaces */
            case b_QU:
                j = i;
                while (j < count-1 && break_class[j+1] == b_SP)
                    j++;
                if (j < count - 1 && break_class[j+1] == b_OP)
                    set_break_condition(j, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                break;
            /* LB16 */
            case b_NS:
                j = i-1;
                while(j > 0 && break_class[j] == b_SP)
                    j--;
                if (break_class[j] == b_CL || break_class[j] == b_CP)
                    for (j++; j <= i; j++)
                        set_break_condition(j, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                break;
            /* LB17 - do not break within B2, even with intervening spaces */
            case b_B2:
                j = i;
                while (j < count && break_class[j+1] == b_SP)
                    j++;
                if (j < count - 1 && break_class[j+1] == b_B2)
                    set_break_condition(j, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                break;
        }
    }

    for (i = 0; i < count; i++)
    {
        switch(break_class[i])
        {
            /* LB18 - break is allowed after space */
            case b_SP:
                set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_CAN_BREAK, &state);
                break;
            /* LB19 - don't break before or after quotation mark */
            case b_QU:
                set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                break;
            /* LB20 */
            case b_CB:
                set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_CAN_BREAK, &state);
                if (i < count - 1 && break_class[i+1] != b_QU)
                    set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_CAN_BREAK, &state);
                break;
            /* LB21 */
            case b_BA:
            case b_HY:
            case b_NS:
                set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                break;
            case b_BB:
                if (i < count - 1 && break_class[i+1] != b_CB)
                    set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                break;
            /* LB21a, LB21b */
            case b_HL:
                /* LB21a */
                if (i < count-1)
                    switch (break_class[i+1])
                    {
                    case b_HY:
                    case b_BA:
                        set_break_condition(i+1, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                    }
                /* LB21b */
                if (i > 0 && break_class[i-1] == b_SY)
                    set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                break;
            /* LB22 */
            case b_IN:
                if (i > 0)
                {
                    switch (break_class[i-1])
                    {
                        case b_AL:
                        case b_HL:
                        case b_EX:
                        case b_ID:
                        case b_EB:
                        case b_EM:
                        case b_IN:
                        case b_NU:
                            set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                    }
                }
                break;
        }

        if (i < count-1)
        {
            /* LB23 - do not break between digits and letters */
            if ((break_class[i] == b_AL && break_class[i+1] == b_NU) ||
                (break_class[i] == b_HL && break_class[i+1] == b_NU) ||
                (break_class[i] == b_NU && break_class[i+1] == b_AL) ||
                (break_class[i] == b_NU && break_class[i+1] == b_HL))
                    set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);

            /* LB23a - do not break between numeric prefixes and ideographs, or between ideographs and numeric postfixes */
            if ((break_class[i] == b_PR && break_class[i+1] == b_ID) ||
                (break_class[i] == b_PR && break_class[i+1] == b_EB) ||
                (break_class[i] == b_PR && break_class[i+1] == b_EM) ||
                (break_class[i] == b_ID && break_class[i+1] == b_PO) ||
                (break_class[i] == b_EM && break_class[i+1] == b_PO) ||
                (break_class[i] == b_EB && break_class[i+1] == b_PO))
                    set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);

            /* LB24 - do not break between numeric prefix/postfix and letters, or letters and prefix/postfix */
            if ((break_class[i] == b_PR && break_class[i+1] == b_AL) ||
                (break_class[i] == b_PR && break_class[i+1] == b_HL) ||
                (break_class[i] == b_PO && break_class[i+1] == b_AL) ||
                (break_class[i] == b_PO && break_class[i+1] == b_HL) ||
                (break_class[i] == b_AL && break_class[i+1] == b_PR) ||
                (break_class[i] == b_HL && break_class[i+1] == b_PR) ||
                (break_class[i] == b_AL && break_class[i+1] == b_PO) ||
                (break_class[i] == b_HL && break_class[i+1] == b_PO))
                    set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);

            /* LB25 */
            if ((break_class[i] == b_CL && break_class[i+1] == b_PO) ||
                (break_class[i] == b_CP && break_class[i+1] == b_PO) ||
                (break_class[i] == b_CL && break_class[i+1] == b_PR) ||
                (break_class[i] == b_CP && break_class[i+1] == b_PR) ||
                (break_class[i] == b_NU && break_class[i+1] == b_PO) ||
                (break_class[i] == b_NU && break_class[i+1] == b_PR) ||
                (break_class[i] == b_PO && break_class[i+1] == b_OP) ||
                (break_class[i] == b_PO && break_class[i+1] == b_NU) ||
                (break_class[i] == b_PR && break_class[i+1] == b_OP) ||
                (break_class[i] == b_PR && break_class[i+1] == b_NU) ||
                (break_class[i] == b_HY && break_class[i+1] == b_NU) ||
                (break_class[i] == b_IS && break_class[i+1] == b_NU) ||
                (break_class[i] == b_NU && break_class[i+1] == b_NU) ||
                (break_class[i] == b_SY && break_class[i+1] == b_NU))
                    set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);

            /* LB26 */
            if (break_class[i] == b_JL)
            {
                switch (break_class[i+1])
                {
                    case b_JL:
                    case b_JV:
                    case b_H2:
                    case b_H3:
                        set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                }
            }
            if ((break_class[i] == b_JV || break_class[i] == b_H2) &&
                (break_class[i+1] == b_JV || break_class[i+1] == b_JT))
                    set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
            if ((break_class[i] == b_JT || break_class[i] == b_H3) &&
                 break_class[i+1] == b_JT)
                    set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);

            /* LB27 */
            switch (break_class[i])
            {
                case b_JL:
                case b_JV:
                case b_JT:
                case b_H2:
                case b_H3:
                    if (break_class[i+1] == b_IN || break_class[i+1] == b_PO)
                        set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
            }
            if (break_class[i] == b_PR)
            {
                switch (break_class[i+1])
                {
                    case b_JL:
                    case b_JV:
                    case b_JT:
                    case b_H2:
                    case b_H3:
                        set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
                }
            }

            /* LB28 */
            if ((break_class[i] == b_AL && break_class[i+1] == b_AL) ||
                (break_class[i] == b_AL && break_class[i+1] == b_HL) ||
                (break_class[i] == b_HL && break_class[i+1] == b_AL) ||
                (break_class[i] == b_HL && break_class[i+1] == b_HL))
                set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);

            /* LB29 */
            if ((break_class[i] == b_IS && break_class[i+1] == b_AL) ||
                (break_class[i] == b_IS && break_class[i+1] == b_HL))
                set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);

            /* LB30 */
            if ((break_class[i] == b_AL || break_class[i] == b_HL || break_class[i] == b_NU) &&
                 break_class[i+1] == b_OP)
                set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
            if (break_class[i] == b_CP &&
               (break_class[i+1] == b_AL || break_class[i+1] == b_HL || break_class[i+1] == b_NU))
                set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);

            /* LB30a - break between two RIs if and only if there are an even number of RIs preceding position of the break */
            if (break_class[i] == b_RI && break_class[i+1] == b_RI) {
                unsigned int c = 0;

                j = i + 1;
                while (j > 0 && break_class[--j] == b_RI)
                    c++;

                if ((c & 1) == 0)
                    set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
            }

            /* LB30b - do not break between an emoji base and an emoji modifier */
            if (break_class[i] == b_EB && break_class[i+1] == b_EM)
                set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_MAY_NOT_BREAK, &state);
        }
    }

    /* LB31 - allow breaks everywhere else. */
    for (i = 0; i < count; i++)
    {
        set_break_condition(i, BreakConditionBefore, DWRITE_BREAK_CONDITION_CAN_BREAK, &state);
        set_break_condition(i, BreakConditionAfter, DWRITE_BREAK_CONDITION_CAN_BREAK, &state);
    }

    heap_free(break_class);
    return S_OK;
}

static HRESULT WINAPI dwritetextanalyzer_QueryInterface(IDWriteTextAnalyzer2 *iface, REFIID riid, void **obj)
{
    TRACE("(%s %p)\n", debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IDWriteTextAnalyzer2) ||
        IsEqualIID(riid, &IID_IDWriteTextAnalyzer1) ||
        IsEqualIID(riid, &IID_IDWriteTextAnalyzer) ||
        IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        return S_OK;
    }

    WARN("%s not implemented.\n", debugstr_guid(riid));

    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dwritetextanalyzer_AddRef(IDWriteTextAnalyzer2 *iface)
{
    return 2;
}

static ULONG WINAPI dwritetextanalyzer_Release(IDWriteTextAnalyzer2 *iface)
{
    return 1;
}

/* This helper tries to get 'length' chars from a source, allocating a buffer only if source failed to provide enough
   data after a first request. */
static HRESULT get_text_source_ptr(IDWriteTextAnalysisSource *source, UINT32 position, UINT32 length, const WCHAR **text, WCHAR **buff)
{
    HRESULT hr;
    UINT32 len;

    *buff = NULL;
    *text = NULL;
    len = 0;
    hr = IDWriteTextAnalysisSource_GetTextAtPosition(source, position, text, &len);
    if (FAILED(hr)) return hr;

    if (len < length) {
        UINT32 read;

        *buff = heap_alloc(length*sizeof(WCHAR));
        if (!*buff)
            return E_OUTOFMEMORY;
        memcpy(*buff, *text, len*sizeof(WCHAR));
        read = len;

        while (read < length && *text) {
            *text = NULL;
            len = 0;
            hr = IDWriteTextAnalysisSource_GetTextAtPosition(source, read, text, &len);
            if (FAILED(hr)) {
                heap_free(*buff);
                return hr;
            }
            memcpy(*buff + read, *text, min(len, length-read)*sizeof(WCHAR));
            read += len;
        }

        *text = *buff;
    }

    return hr;
}

static HRESULT WINAPI dwritetextanalyzer_AnalyzeScript(IDWriteTextAnalyzer2 *iface,
    IDWriteTextAnalysisSource* source, UINT32 position, UINT32 length, IDWriteTextAnalysisSink* sink)
{
    WCHAR *buff = NULL;
    const WCHAR *text;
    HRESULT hr;

    TRACE("(%p %u %u %p)\n", source, position, length, sink);

    if (length == 0)
        return S_OK;

    hr = get_text_source_ptr(source, position, length, &text, &buff);
    if (FAILED(hr))
        return hr;

    hr = analyze_script(text, position, length, sink);
    heap_free(buff);

    return hr;
}

static HRESULT WINAPI dwritetextanalyzer_AnalyzeBidi(IDWriteTextAnalyzer2 *iface,
    IDWriteTextAnalysisSource* source, UINT32 position, UINT32 length, IDWriteTextAnalysisSink* sink)
{
    UINT8 *levels = NULL, *explicit = NULL;
    UINT8 baselevel, level, explicit_level;
    UINT32 pos, i, seq_length;
    WCHAR *buff = NULL;
    const WCHAR *text;
    HRESULT hr;

    TRACE("(%p %u %u %p)\n", source, position, length, sink);

    if (!length)
        return S_OK;

    hr = get_text_source_ptr(source, position, length, &text, &buff);
    if (FAILED(hr))
        return hr;

    levels = heap_calloc(length, sizeof(*levels));
    explicit = heap_calloc(length, sizeof(*explicit));

    if (!levels || !explicit) {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    baselevel = IDWriteTextAnalysisSource_GetParagraphReadingDirection(source);
    hr = bidi_computelevels(text, length, baselevel, explicit, levels);
    if (FAILED(hr))
        goto done;

    level = levels[0];
    explicit_level = explicit[0];
    pos = position;
    seq_length = 1;

    for (i = 1; i < length; i++) {
        if (levels[i] == level && explicit[i] == explicit_level)
            seq_length++;
        else {
            hr = IDWriteTextAnalysisSink_SetBidiLevel(sink, pos, seq_length, explicit_level, level);
            if (FAILED(hr))
                goto done;

            pos += seq_length;
            seq_length = 1;
            level = levels[i];
            explicit_level = explicit[i];
        }
    }
    /* one char length case or normal completion call */
    hr = IDWriteTextAnalysisSink_SetBidiLevel(sink, pos, seq_length, explicit_level, level);

done:
    heap_free(explicit);
    heap_free(levels);
    heap_free(buff);

    return hr;
}

static HRESULT WINAPI dwritetextanalyzer_AnalyzeNumberSubstitution(IDWriteTextAnalyzer2 *iface,
    IDWriteTextAnalysisSource* source, UINT32 position, UINT32 length, IDWriteTextAnalysisSink* sink)
{
    static int once;

    if (!once++)
        FIXME("(%p %u %u %p): stub\n", source, position, length, sink);
    return S_OK;
}

static HRESULT WINAPI dwritetextanalyzer_AnalyzeLineBreakpoints(IDWriteTextAnalyzer2 *iface,
    IDWriteTextAnalysisSource* source, UINT32 position, UINT32 length, IDWriteTextAnalysisSink* sink)
{
    DWRITE_LINE_BREAKPOINT *breakpoints = NULL;
    WCHAR *buff = NULL;
    const WCHAR *text;
    HRESULT hr;
    UINT32 len;

    TRACE("(%p %u %u %p)\n", source, position, length, sink);

    if (length == 0)
        return S_OK;

    /* get some, check for length */
    text = NULL;
    len = 0;
    hr = IDWriteTextAnalysisSource_GetTextAtPosition(source, position, &text, &len);
    if (FAILED(hr)) return hr;

    if (len < length) {
        UINT32 read;

        buff = heap_calloc(length, sizeof(*buff));
        if (!buff)
            return E_OUTOFMEMORY;
        memcpy(buff, text, len*sizeof(WCHAR));
        read = len;

        while (read < length && text) {
            text = NULL;
            len = 0;
            hr = IDWriteTextAnalysisSource_GetTextAtPosition(source, read, &text, &len);
            if (FAILED(hr))
                goto done;
            memcpy(&buff[read], text, min(len, length-read)*sizeof(WCHAR));
            read += len;
        }

        text = buff;
    }

    breakpoints = heap_calloc(length, sizeof(*breakpoints));
    if (!breakpoints) {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    hr = analyze_linebreaks(text, length, breakpoints);
    if (FAILED(hr))
        goto done;

    hr = IDWriteTextAnalysisSink_SetLineBreakpoints(sink, position, length, breakpoints);

done:
    heap_free(breakpoints);
    heap_free(buff);

    return hr;
}

static UINT32 get_opentype_language(const WCHAR *locale)
{
    UINT32 language = DWRITE_MAKE_OPENTYPE_TAG('d','f','l','t');

    if (locale) {
        WCHAR tag[5];
        if (GetLocaleInfoEx(locale, LOCALE_SOPENTYPELANGUAGETAG, tag, ARRAY_SIZE(tag)))
            language = DWRITE_MAKE_OPENTYPE_TAG(tag[0],tag[1],tag[2],tag[3]);
    }

    return language;
}

static DWRITE_NUMBER_SUBSTITUTION_METHOD get_number_substitutes(IDWriteNumberSubstitution *substitution, WCHAR *digits)
{
    struct dwrite_numbersubstitution *numbersubst = unsafe_impl_from_IDWriteNumberSubstitution(substitution);
    DWRITE_NUMBER_SUBSTITUTION_METHOD method;
    WCHAR isolang[9];
    DWORD lctype;

    if (!numbersubst)
        return DWRITE_NUMBER_SUBSTITUTION_METHOD_NONE;

    lctype = numbersubst->ignore_user_override ? LOCALE_NOUSEROVERRIDE : 0;

    if (numbersubst->method == DWRITE_NUMBER_SUBSTITUTION_METHOD_FROM_CULTURE) {
        DWORD value;

        method = DWRITE_NUMBER_SUBSTITUTION_METHOD_NONE;
        if (GetLocaleInfoEx(numbersubst->locale, lctype | LOCALE_IDIGITSUBSTITUTION | LOCALE_RETURN_NUMBER, (WCHAR *)&value, 2)) {
            switch (value)
            {
            case 0:
                method = DWRITE_NUMBER_SUBSTITUTION_METHOD_CONTEXTUAL;
                break;
            case 2:
                method = DWRITE_NUMBER_SUBSTITUTION_METHOD_NATIONAL;
                break;
            case 1:
            default:
                if (value != 1)
                    WARN("Unknown IDIGITSUBSTITUTION value %u, locale %s.\n", value, debugstr_w(numbersubst->locale));
            }
        }
        else
            WARN("Failed to get IDIGITSUBSTITUTION for locale %s\n", debugstr_w(numbersubst->locale));
    }
    else
        method = numbersubst->method;

    digits[0] = 0;
    switch (method)
    {
    case DWRITE_NUMBER_SUBSTITUTION_METHOD_NATIONAL:
        GetLocaleInfoEx(numbersubst->locale, lctype | LOCALE_SNATIVEDIGITS, digits, NATIVE_DIGITS_LEN);
        break;
    case DWRITE_NUMBER_SUBSTITUTION_METHOD_CONTEXTUAL:
    case DWRITE_NUMBER_SUBSTITUTION_METHOD_TRADITIONAL:
        if (GetLocaleInfoEx(numbersubst->locale, LOCALE_SISO639LANGNAME, isolang, ARRAY_SIZE(isolang))) {
             static const WCHAR arW[] = {'a','r',0};
             static const WCHAR arabicW[] = {0x640,0x641,0x642,0x643,0x644,0x645,0x646,0x647,0x648,0x649,0};

             /* For some Arabic locales Latin digits are returned for SNATIVEDIGITS */
             if (!strcmpW(arW, isolang)) {
                 strcpyW(digits, arabicW);
                 break;
             }
        }
        GetLocaleInfoEx(numbersubst->locale, lctype | LOCALE_SNATIVEDIGITS, digits, NATIVE_DIGITS_LEN);
        break;
    default:
        ;
    }

    if (method != DWRITE_NUMBER_SUBSTITUTION_METHOD_NONE && !*digits) {
        WARN("Failed to get number substitutes for locale %s, method %d\n", debugstr_w(numbersubst->locale), method);
        method = DWRITE_NUMBER_SUBSTITUTION_METHOD_NONE;
    }

    return method;
}

static void analyzer_dump_user_features(DWRITE_TYPOGRAPHIC_FEATURES const **features,
        UINT32 const *feature_range_lengths, UINT32 feature_ranges)
{
    UINT32 i, j, start;

    if (!TRACE_ON(dwrite) || !features)
        return;

    for (i = 0, start = 0; i < feature_ranges; start += feature_range_lengths[i++]) {
        TRACE("feature range [%u,%u)\n", start, start + feature_range_lengths[i]);
        for (j = 0; j < features[i]->featureCount; j++)
            TRACE("feature %s, parameter %u\n", debugstr_tag(features[i]->features[j].nameTag),
                    features[i]->features[j].parameter);
    }
}

static HRESULT WINAPI dwritetextanalyzer_GetGlyphs(IDWriteTextAnalyzer2 *iface,
    WCHAR const* text, UINT32 length, IDWriteFontFace* fontface, BOOL is_sideways,
    BOOL is_rtl, DWRITE_SCRIPT_ANALYSIS const* analysis, WCHAR const* locale,
    IDWriteNumberSubstitution* substitution, DWRITE_TYPOGRAPHIC_FEATURES const** features,
    UINT32 const* feature_range_lengths, UINT32 feature_ranges, UINT32 max_glyph_count,
    UINT16* clustermap, DWRITE_SHAPING_TEXT_PROPERTIES* text_props, UINT16* glyph_indices,
    DWRITE_SHAPING_GLYPH_PROPERTIES* glyph_props, UINT32* actual_glyph_count)
{
    DWRITE_NUMBER_SUBSTITUTION_METHOD method;
    struct scriptshaping_context context;
    struct dwrite_fontface *font_obj;
    WCHAR digits[NATIVE_DIGITS_LEN];
    BOOL update_cluster;
    WCHAR *string;
    UINT32 i, g;
    HRESULT hr = S_OK;

    TRACE("(%s:%u %p %d %d %s %s %p %p %p %u %u %p %p %p %p %p)\n", debugstr_wn(text, length),
        length, fontface, is_sideways, is_rtl, debugstr_sa_script(analysis->script), debugstr_w(locale), substitution,
        features, feature_range_lengths, feature_ranges, max_glyph_count, clustermap, text_props, glyph_indices,
        glyph_props, actual_glyph_count);

    analyzer_dump_user_features(features, feature_range_lengths, feature_ranges);

    if (max_glyph_count < length)
        return E_NOT_SUFFICIENT_BUFFER;

    string = heap_calloc(length, sizeof(*string));
    if (!string)
        return E_OUTOFMEMORY;

    method = get_number_substitutes(substitution, digits);

    for (i = 0; i < length; i++) {
        /* FIXME: set to better values */
        glyph_props[i].justification = text[i] == ' ' ? SCRIPT_JUSTIFY_BLANK : SCRIPT_JUSTIFY_CHARACTER;
        glyph_props[i].isClusterStart = 1;
        glyph_props[i].isDiacritic = 0;
        glyph_props[i].isZeroWidthSpace = 0;
        glyph_props[i].reserved = 0;

        /* FIXME: have the shaping engine set this */
        text_props[i].isShapedAlone = 0;
        text_props[i].reserved = 0;

        clustermap[i] = i;

        string[i] = text[i];
        switch (method)
        {
        case DWRITE_NUMBER_SUBSTITUTION_METHOD_CONTEXTUAL:
            if (!is_rtl)
                break;
            /* fallthrough */
        default:
            if (string[i] >= '0' && string[i] <= '9')
                string[i] = digits[string[i] - '0'];
            break;
        case DWRITE_NUMBER_SUBSTITUTION_METHOD_NONE:
            ;
        }
    }

    for (; i < max_glyph_count; i++) {
        glyph_props[i].justification = SCRIPT_JUSTIFY_NONE;
        glyph_props[i].isClusterStart = 0;
        glyph_props[i].isDiacritic = 0;
        glyph_props[i].isZeroWidthSpace = 0;
        glyph_props[i].reserved = 0;
    }

    for (i = 0, g = 0, update_cluster = FALSE; i < length; i++) {
        UINT32 codepoint;

        if (!update_cluster) {
            codepoint = decode_surrogate_pair(string, i, length);
            if (!codepoint)
                codepoint = is_rtl ? bidi_get_mirrored_char(string[i]) : string[i];
            else
                update_cluster = TRUE;

            hr = IDWriteFontFace_GetGlyphIndices(fontface, &codepoint, 1, &glyph_indices[g]);
            if (FAILED(hr))
                goto done;

            g++;
        }
        else {
            INT32 k;

            update_cluster = FALSE;
            /* mark surrogate halves with same cluster */
            clustermap[i] = clustermap[i-1];
            /* update following clusters */
            for (k = i + 1; k >= 0 && k < length; k++)
                clustermap[k]--;
        }
    }
    *actual_glyph_count = g;

    font_obj = unsafe_impl_from_IDWriteFontFace(fontface);

    context.cache = fontface_get_shaping_cache(font_obj);
    context.text = text;
    context.length = length;
    context.is_rtl = is_rtl;
    context.language_tag = get_opentype_language(locale);

    /* FIXME: apply default features */

    hr = default_shaping_ops.set_text_glyphs_props(&context, clustermap, glyph_indices, *actual_glyph_count, text_props, glyph_props);

done:
    heap_free(string);

    return hr;
}

static HRESULT WINAPI dwritetextanalyzer_GetGlyphPlacements(IDWriteTextAnalyzer2 *iface,
    WCHAR const* text, UINT16 const* clustermap, DWRITE_SHAPING_TEXT_PROPERTIES* props,
    UINT32 text_len, UINT16 const* glyphs, DWRITE_SHAPING_GLYPH_PROPERTIES const* glyph_props,
    UINT32 glyph_count, IDWriteFontFace *fontface, float emSize, BOOL is_sideways, BOOL is_rtl,
    DWRITE_SCRIPT_ANALYSIS const* analysis, WCHAR const* locale, DWRITE_TYPOGRAPHIC_FEATURES const** features,
    UINT32 const* feature_range_lengths, UINT32 feature_ranges, float *advances, DWRITE_GLYPH_OFFSET *offsets)
{
    const struct dwritescript_properties *scriptprops;
    struct dwrite_fontface *font_obj;
    unsigned int i, script;
    HRESULT hr = S_OK;

    TRACE("(%s %p %p %u %p %p %u %p %.2f %d %d %s %s %p %p %u %p %p)\n", debugstr_wn(text, text_len),
        clustermap, props, text_len, glyphs, glyph_props, glyph_count, fontface, emSize, is_sideways,
        is_rtl, debugstr_sa_script(analysis->script), debugstr_w(locale), features, feature_range_lengths,
        feature_ranges, advances, offsets);

    analyzer_dump_user_features(features, feature_range_lengths, feature_ranges);

    if (glyph_count == 0)
        return S_OK;

    font_obj = unsafe_impl_from_IDWriteFontFace(fontface);

    for (i = 0; i < glyph_count; ++i)
    {
        if (glyph_props[i].isZeroWidthSpace)
            advances[i] = 0.0f;
        else
            advances[i] = fontface_get_scaled_design_advance(font_obj, DWRITE_MEASURING_MODE_NATURAL, emSize, 1.0f,
                    NULL, glyphs[i], is_sideways);
        offsets[i].advanceOffset = 0.0f;
        offsets[i].ascenderOffset = 0.0f;
    }

    script = analysis->script > Script_LastId ? Script_Unknown : analysis->script;

    scriptprops = &dwritescripts_properties[script];
    if (scriptprops->ops && scriptprops->ops->gpos_features)
    {
        struct scriptshaping_context context;

        context.cache = fontface_get_shaping_cache(font_obj);
        context.text = text;
        context.length = text_len;
        context.is_rtl = is_rtl;
        context.u.pos.glyphs = glyphs;
        context.u.pos.glyph_props = glyph_props;
        context.glyph_count = glyph_count;
        context.emsize = emSize;
        context.measuring_mode = DWRITE_MEASURING_MODE_NATURAL;
        context.advances = advances;
        context.offsets = offsets;
        context.language_tag = get_opentype_language(locale);

        hr = shape_get_positions(&context, scriptprops->scripttags, scriptprops->ops->gpos_features);
    }

    return hr;
}

static HRESULT WINAPI dwritetextanalyzer_GetGdiCompatibleGlyphPlacements(IDWriteTextAnalyzer2 *iface,
    WCHAR const* text, UINT16 const* clustermap, DWRITE_SHAPING_TEXT_PROPERTIES* props,
    UINT32 text_len, UINT16 const* glyphs, DWRITE_SHAPING_GLYPH_PROPERTIES const* glyph_props,
    UINT32 glyph_count, IDWriteFontFace *fontface, float emSize, float ppdip,
    DWRITE_MATRIX const* transform, BOOL use_gdi_natural, BOOL is_sideways, BOOL is_rtl,
    DWRITE_SCRIPT_ANALYSIS const* analysis, WCHAR const* locale, DWRITE_TYPOGRAPHIC_FEATURES const** features,
    UINT32 const* feature_range_lengths, UINT32 feature_ranges, float *advances, DWRITE_GLYPH_OFFSET *offsets)
{
    const struct dwritescript_properties *scriptprops;
    DWRITE_MEASURING_MODE measuring_mode;
    struct dwrite_fontface *font_obj;
    unsigned int i, script;
    HRESULT hr = S_OK;

    TRACE("(%s %p %p %u %p %p %u %p %.2f %.2f %p %d %d %d %s %s %p %p %u %p %p)\n", debugstr_wn(text, text_len),
        clustermap, props, text_len, glyphs, glyph_props, glyph_count, fontface, emSize, ppdip,
        transform, use_gdi_natural, is_sideways, is_rtl, debugstr_sa_script(analysis->script), debugstr_w(locale),
        features, feature_range_lengths, feature_ranges, advances, offsets);

    analyzer_dump_user_features(features, feature_range_lengths, feature_ranges);

    if (glyph_count == 0)
        return S_OK;

    font_obj = unsafe_impl_from_IDWriteFontFace(fontface);

    measuring_mode = use_gdi_natural ? DWRITE_MEASURING_MODE_GDI_NATURAL : DWRITE_MEASURING_MODE_GDI_CLASSIC;

    for (i = 0; i < glyph_count; ++i)
    {
        if (glyph_props[i].isZeroWidthSpace)
            advances[i] = 0.0f;
        else
            advances[i] = fontface_get_scaled_design_advance(font_obj, measuring_mode, emSize, ppdip,
                    transform, glyphs[i], is_sideways);
        offsets[i].advanceOffset = 0.0f;
        offsets[i].ascenderOffset = 0.0f;
    }

    script = analysis->script > Script_LastId ? Script_Unknown : analysis->script;

    scriptprops = &dwritescripts_properties[script];
    if (scriptprops->ops && scriptprops->ops->gpos_features)
    {
        struct scriptshaping_context context;

        context.cache = fontface_get_shaping_cache(font_obj);
        context.text = text;
        context.length = text_len;
        context.is_rtl = is_rtl;
        context.u.pos.glyphs = glyphs;
        context.u.pos.glyph_props = glyph_props;
        context.glyph_count = glyph_count;
        context.emsize = emSize * ppdip;
        context.measuring_mode = measuring_mode;
        context.advances = advances;
        context.offsets = offsets;
        context.language_tag = get_opentype_language(locale);

        hr = shape_get_positions(&context, scriptprops->scripttags, scriptprops->ops->gpos_features);
    }

    return hr;
}

static inline FLOAT get_cluster_advance(const FLOAT *advances, UINT32 start, UINT32 end)
{
    FLOAT advance = 0.0f;
    for (; start < end; start++)
        advance += advances[start];
    return advance;
}

static HRESULT apply_cluster_spacing(float leading_spacing, float trailing_spacing, float min_advance_width,
        unsigned int start, unsigned int end, float const *advances, DWRITE_GLYPH_OFFSET const *offsets,
        DWRITE_SHAPING_GLYPH_PROPERTIES const *glyph_props, float *modified_advances,
        DWRITE_GLYPH_OFFSET *modified_offsets)
{
    BOOL reduced = leading_spacing < 0.0f || trailing_spacing < 0.0f;
    unsigned int first_spacing, last_spacing, i;
    float advance, origin = 0.0f, *deltas;
    BOOL is_spacing_cluster = FALSE;

    if (modified_advances != advances)
        memcpy(&modified_advances[start], &advances[start], (end - start + 1) * sizeof(*advances));
    if (modified_offsets != offsets)
        memcpy(&modified_offsets[start], &offsets[start], (end - start + 1) * sizeof(*offsets));

    for (first_spacing = start; first_spacing <= end; ++first_spacing)
    {
        if ((is_spacing_cluster = !glyph_props[first_spacing].isZeroWidthSpace))
            break;
    }

    /* Nothing to adjust if there is no spacing glyphs. */
    if (!is_spacing_cluster)
        return S_OK;

    for (last_spacing = end; last_spacing >= start; --last_spacing)
    {
        if (!glyph_props[last_spacing].isZeroWidthSpace)
            break;
    }

    deltas = heap_calloc(end - start + 1, sizeof(*deltas));
    if (!deltas)
        return E_OUTOFMEMORY;

    /* Cluster advance, note that properties are ignored. */
    origin = offsets[start].advanceOffset;
    for (i = start, advance = 0.0f; i <= end; ++i)
    {
        float cur = advance + offsets[i].advanceOffset;

        deltas[i - start] = cur - origin;

        advance += advances[i];
        origin = cur;
    }

    /* Negative spacing. */
    if (leading_spacing < 0.0f)
    {
        advance += leading_spacing;
        modified_advances[first_spacing] += leading_spacing;
        modified_offsets[first_spacing].advanceOffset += leading_spacing;
    }

    if (trailing_spacing < 0.0f)
    {
        advance += trailing_spacing;
        modified_advances[last_spacing] += trailing_spacing;
    }

    /* Minimal advance. */
    advance = min_advance_width - advance;
    if (advance > 0.0f) {
        /* Additional spacing is only applied to leading and trailing spacing glyphs. */
        float half = advance / 2.0f;

        if (!reduced)
        {
            modified_advances[first_spacing] += half;
            modified_advances[last_spacing] += half;
            modified_offsets[first_spacing].advanceOffset += half;
        }
        else if (leading_spacing < 0.0f && trailing_spacing < 0.0f)
        {
            modified_advances[first_spacing] += half;
            modified_advances[last_spacing] += half;
            modified_offsets[first_spacing].advanceOffset += half;
        }
        else if (leading_spacing < 0.0f)
        {
            modified_advances[first_spacing] += advance;
            modified_offsets[first_spacing].advanceOffset += advance;
        }
        else
            modified_advances[last_spacing] += advance;
    }

    /* Positive spacing. */
    if (leading_spacing > 0.0f)
    {
        modified_advances[first_spacing] += leading_spacing;
        modified_offsets[first_spacing].advanceOffset += leading_spacing;
    }

    if (trailing_spacing > 0.0f)
        modified_advances[last_spacing] += trailing_spacing;

    /* Update offsets to preserve original relative positions within cluster. */
    for (i = first_spacing; i > start; --i)
    {
        unsigned int cur = i - 1;
        modified_offsets[cur].advanceOffset = modified_advances[cur] + modified_offsets[i].advanceOffset -
                deltas[i - start];
    }

    for (i = first_spacing + 1; i <= end; ++i)
    {
        modified_offsets[i].advanceOffset = deltas[i - start] + modified_offsets[i - 1].advanceOffset -
                modified_advances[i - 1];
    }

    heap_free(deltas);

    return S_OK;
}

static inline UINT32 get_cluster_length(UINT16 const *clustermap, UINT32 start, UINT32 text_len)
{
    UINT16 g = clustermap[start];
    UINT32 length = 1;

    while (start < text_len && clustermap[++start] == g)
        length++;
    return length;
}

/* Applies spacing adjustments to clusters.

   Adjustments are applied in the following order:

   1. Negative adjustments

      Leading and trailing spacing could be negative, at this step
      only negative ones are actually applied. Leading spacing is only
      applied to leading glyph, trailing - to trailing glyph.

   2. Minimum advance width

      Advances could only be reduced at this point or unchanged. In any
      case it's checked if cluster advance width is less than minimum width.
      If it's the case advance width is incremented up to minimum value.

      Important part is the direction in which this increment is applied;
      it depends on direction from which total cluster advance was trimmed
      at step 1. So it could be incremented from leading, trailing, or both
      sides. When applied to both sides, each side gets half of difference
      that brings advance to minimum width.

   3. Positive adjustments

      After minimum width rule was applied, positive spacing is applied in the same
      way as negative one on step 1.

   Glyph offset for leading glyph is adjusted too in a way that glyph origin
   keeps its position in coordinate system where initial advance width is counted
   from 0.

   Glyph properties

   It's known that isZeroWidthSpace property keeps initial advance from changing.

*/
static HRESULT WINAPI dwritetextanalyzer1_ApplyCharacterSpacing(IDWriteTextAnalyzer2 *iface,
    FLOAT leading_spacing, FLOAT trailing_spacing, FLOAT min_advance_width, UINT32 len,
    UINT32 glyph_count, UINT16 const *clustermap, FLOAT const *advances, DWRITE_GLYPH_OFFSET const *offsets,
    DWRITE_SHAPING_GLYPH_PROPERTIES const *props, FLOAT *modified_advances, DWRITE_GLYPH_OFFSET *modified_offsets)
{
    unsigned int i;

    TRACE("(%.2f %.2f %.2f %u %u %p %p %p %p %p %p)\n", leading_spacing, trailing_spacing, min_advance_width,
        len, glyph_count, clustermap, advances, offsets, props, modified_advances, modified_offsets);

    if (min_advance_width < 0.0f) {
        if (modified_advances != advances)
            memset(modified_advances, 0, glyph_count*sizeof(*modified_advances));
        return E_INVALIDARG;
    }

    for (i = 0; i < len;)
    {
        unsigned int length = get_cluster_length(clustermap, i, len);
        unsigned int start, end;

        start = clustermap[i];
        end = i + length < len ? clustermap[i + length] : glyph_count;

        apply_cluster_spacing(leading_spacing, trailing_spacing, min_advance_width, start, end - 1, advances,
                offsets, props, modified_advances, modified_offsets);

        i += length;
    }

    return S_OK;
}

static HRESULT WINAPI dwritetextanalyzer1_GetBaseline(IDWriteTextAnalyzer2 *iface, IDWriteFontFace *face,
    DWRITE_BASELINE baseline, BOOL vertical, BOOL is_simulation_allowed, DWRITE_SCRIPT_ANALYSIS sa,
    const WCHAR *localeName, INT32 *baseline_coord, BOOL *exists)
{
    FIXME("(%p %d %d %u %s %p %p): stub\n", face, vertical, is_simulation_allowed, sa.script, debugstr_w(localeName),
        baseline_coord, exists);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextanalyzer1_AnalyzeVerticalGlyphOrientation(IDWriteTextAnalyzer2 *iface,
    IDWriteTextAnalysisSource1* source, UINT32 text_pos, UINT32 len, IDWriteTextAnalysisSink1 *sink)
{
    FIXME("(%p %u %u %p): stub\n", source, text_pos, len, sink);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextanalyzer1_GetGlyphOrientationTransform(IDWriteTextAnalyzer2 *iface,
    DWRITE_GLYPH_ORIENTATION_ANGLE angle, BOOL is_sideways, DWRITE_MATRIX *transform)
{
    TRACE("(%d %d %p)\n", angle, is_sideways, transform);
    return IDWriteTextAnalyzer2_GetGlyphOrientationTransform(iface, angle, is_sideways, 0.0, 0.0, transform);
}

static HRESULT WINAPI dwritetextanalyzer1_GetScriptProperties(IDWriteTextAnalyzer2 *iface, DWRITE_SCRIPT_ANALYSIS sa,
    DWRITE_SCRIPT_PROPERTIES *props)
{
    TRACE("(%u %p)\n", sa.script, props);

    if (sa.script > Script_LastId)
        return E_INVALIDARG;

    *props = dwritescripts_properties[sa.script].props;
    return S_OK;
}

static inline BOOL is_char_from_simple_script(WCHAR c)
{
    if (IS_HIGH_SURROGATE(c) || IS_LOW_SURROGATE(c) ||
            /* LRM, RLM, LRE, RLE, PDF, LRO, RLO */
            c == 0x200e || c == 0x200f || (c >= 0x202a && c <= 0x202e))
        return FALSE;
    else {
        UINT16 script = get_char_script(c);
        return !dwritescripts_properties[script].is_complex;
    }
}

static HRESULT WINAPI dwritetextanalyzer1_GetTextComplexity(IDWriteTextAnalyzer2 *iface, const WCHAR *text,
    UINT32 len, IDWriteFontFace *face, BOOL *is_simple, UINT32 *len_read, UINT16 *indices)
{
    HRESULT hr = S_OK;
    int i;

    TRACE("(%s:%u %p %p %p %p)\n", debugstr_wn(text, len), len, face, is_simple, len_read, indices);

    *is_simple = FALSE;
    *len_read = 0;

    if (!face)
        return E_INVALIDARG;

    if (len == 0) {
        *is_simple = TRUE;
        return S_OK;
    }

    *is_simple = text[0] && is_char_from_simple_script(text[0]);
    for (i = 1; i < len && text[i]; i++) {
        if (is_char_from_simple_script(text[i])) {
            if (!*is_simple)
                break;
        }
        else
            *is_simple = FALSE;
    }

    *len_read = i;

    /* fetch indices */
    if (*is_simple && indices) {
        UINT32 *codepoints = heap_calloc(*len_read, sizeof(*codepoints));
        if (!codepoints)
            return E_OUTOFMEMORY;

        for (i = 0; i < *len_read; i++)
            codepoints[i] = text[i];

        hr = IDWriteFontFace_GetGlyphIndices(face, codepoints, *len_read, indices);
        heap_free(codepoints);
    }

    return hr;
}

static HRESULT WINAPI dwritetextanalyzer1_GetJustificationOpportunities(IDWriteTextAnalyzer2 *iface,
    IDWriteFontFace *face, FLOAT font_em_size, DWRITE_SCRIPT_ANALYSIS sa, UINT32 length, UINT32 glyph_count,
    const WCHAR *text, const UINT16 *clustermap, const DWRITE_SHAPING_GLYPH_PROPERTIES *prop, DWRITE_JUSTIFICATION_OPPORTUNITY *jo)
{
    FIXME("(%p %.2f %u %u %u %s %p %p %p): stub\n", face, font_em_size, sa.script, length, glyph_count,
        debugstr_wn(text, length), clustermap, prop, jo);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextanalyzer1_JustifyGlyphAdvances(IDWriteTextAnalyzer2 *iface,
    FLOAT width, UINT32 glyph_count, const DWRITE_JUSTIFICATION_OPPORTUNITY *jo, const FLOAT *advances,
    const DWRITE_GLYPH_OFFSET *offsets, FLOAT *justifiedadvances, DWRITE_GLYPH_OFFSET *justifiedoffsets)
{
    FIXME("(%.2f %u %p %p %p %p %p): stub\n", width, glyph_count, jo, advances, offsets, justifiedadvances,
        justifiedoffsets);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextanalyzer1_GetJustifiedGlyphs(IDWriteTextAnalyzer2 *iface,
    IDWriteFontFace *face, FLOAT font_em_size, DWRITE_SCRIPT_ANALYSIS sa, UINT32 length,
    UINT32 glyph_count, UINT32 max_glyphcount, const UINT16 *clustermap, const UINT16 *indices,
    const FLOAT *advances, const FLOAT *justifiedadvances, const DWRITE_GLYPH_OFFSET *justifiedoffsets,
    const DWRITE_SHAPING_GLYPH_PROPERTIES *prop, UINT32 *actual_count, UINT16 *modified_clustermap,
    UINT16 *modified_indices, FLOAT *modified_advances, DWRITE_GLYPH_OFFSET *modified_offsets)
{
    FIXME("(%p %.2f %u %u %u %u %p %p %p %p %p %p %p %p %p %p %p): stub\n", face, font_em_size, sa.script,
        length, glyph_count, max_glyphcount, clustermap, indices, advances, justifiedadvances, justifiedoffsets,
        prop, actual_count, modified_clustermap, modified_indices, modified_advances, modified_offsets);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextanalyzer2_GetGlyphOrientationTransform(IDWriteTextAnalyzer2 *iface,
    DWRITE_GLYPH_ORIENTATION_ANGLE angle, BOOL is_sideways, FLOAT originX, FLOAT originY, DWRITE_MATRIX *m)
{
    static const DWRITE_MATRIX transforms[] = {
        {  1.0f,  0.0f,  0.0f,  1.0f, 0.0f, 0.0f },
        {  0.0f,  1.0f, -1.0f,  0.0f, 0.0f, 0.0f },
        { -1.0f,  0.0f,  0.0f, -1.0f, 0.0f, 0.0f },
        {  0.0f, -1.0f,  1.0f,  0.0f, 0.0f, 0.0f }
    };

    TRACE("(%d %d %.2f %.2f %p)\n", angle, is_sideways, originX, originY, m);

    if ((UINT32)angle > DWRITE_GLYPH_ORIENTATION_ANGLE_270_DEGREES) {
        memset(m, 0, sizeof(*m));
        return E_INVALIDARG;
    }

    /* for sideways case simply rotate 90 degrees more */
    if (is_sideways) {
        switch (angle) {
        case DWRITE_GLYPH_ORIENTATION_ANGLE_0_DEGREES:
            angle = DWRITE_GLYPH_ORIENTATION_ANGLE_90_DEGREES;
            break;
        case DWRITE_GLYPH_ORIENTATION_ANGLE_90_DEGREES:
            angle = DWRITE_GLYPH_ORIENTATION_ANGLE_180_DEGREES;
            break;
        case DWRITE_GLYPH_ORIENTATION_ANGLE_180_DEGREES:
            angle = DWRITE_GLYPH_ORIENTATION_ANGLE_270_DEGREES;
            break;
        case DWRITE_GLYPH_ORIENTATION_ANGLE_270_DEGREES:
            angle = DWRITE_GLYPH_ORIENTATION_ANGLE_0_DEGREES;
            break;
        default:
            ;
        }
    }

    *m = transforms[angle];

    /* shift components represent transform necessary to get from original point to
       rotated one in new coordinate system */
    if ((originX != 0.0f || originY != 0.0f) && angle != DWRITE_GLYPH_ORIENTATION_ANGLE_0_DEGREES) {
        m->dx = originX - (m->m11 * originX + m->m21 * originY);
        m->dy = originY - (m->m12 * originX + m->m22 * originY);
    }

    return S_OK;
}

static HRESULT WINAPI dwritetextanalyzer2_GetTypographicFeatures(IDWriteTextAnalyzer2 *iface,
    IDWriteFontFace *fontface, DWRITE_SCRIPT_ANALYSIS sa, const WCHAR *locale,
    UINT32 max_tagcount, UINT32 *actual_tagcount, DWRITE_FONT_FEATURE_TAG *tags)
{
    const struct dwritescript_properties *props;
    const DWORD *scripts;
    HRESULT hr = S_OK;
    UINT32 language;

    TRACE("(%p %u %s %u %p %p)\n", fontface, sa.script, debugstr_w(locale), max_tagcount, actual_tagcount,
        tags);

    if (sa.script > Script_LastId)
        return E_INVALIDARG;

    language = get_opentype_language(locale);
    props = &dwritescripts_properties[sa.script];
    *actual_tagcount = 0;

    scripts = props->scripttags;
    while (*scripts && !*actual_tagcount)
    {
        hr = opentype_get_typographic_features(fontface, *scripts, language, max_tagcount, actual_tagcount, tags);
        scripts++;
    }

    return hr;
};

static HRESULT WINAPI dwritetextanalyzer2_CheckTypographicFeature(IDWriteTextAnalyzer2 *iface,
        IDWriteFontFace *face, DWRITE_SCRIPT_ANALYSIS sa, const WCHAR *locale, DWRITE_FONT_FEATURE_TAG feature,
        UINT32 glyph_count, const UINT16 *glyphs, UINT8 *feature_applies)
{
    FIXME("(%p %u %s %s %u %p %p): stub\n", face, sa.script, debugstr_w(locale), debugstr_tag(feature), glyph_count,
            glyphs, feature_applies);
    return E_NOTIMPL;
}

static const struct IDWriteTextAnalyzer2Vtbl textanalyzervtbl = {
    dwritetextanalyzer_QueryInterface,
    dwritetextanalyzer_AddRef,
    dwritetextanalyzer_Release,
    dwritetextanalyzer_AnalyzeScript,
    dwritetextanalyzer_AnalyzeBidi,
    dwritetextanalyzer_AnalyzeNumberSubstitution,
    dwritetextanalyzer_AnalyzeLineBreakpoints,
    dwritetextanalyzer_GetGlyphs,
    dwritetextanalyzer_GetGlyphPlacements,
    dwritetextanalyzer_GetGdiCompatibleGlyphPlacements,
    dwritetextanalyzer1_ApplyCharacterSpacing,
    dwritetextanalyzer1_GetBaseline,
    dwritetextanalyzer1_AnalyzeVerticalGlyphOrientation,
    dwritetextanalyzer1_GetGlyphOrientationTransform,
    dwritetextanalyzer1_GetScriptProperties,
    dwritetextanalyzer1_GetTextComplexity,
    dwritetextanalyzer1_GetJustificationOpportunities,
    dwritetextanalyzer1_JustifyGlyphAdvances,
    dwritetextanalyzer1_GetJustifiedGlyphs,
    dwritetextanalyzer2_GetGlyphOrientationTransform,
    dwritetextanalyzer2_GetTypographicFeatures,
    dwritetextanalyzer2_CheckTypographicFeature
};

static IDWriteTextAnalyzer2 textanalyzer = { &textanalyzervtbl };

IDWriteTextAnalyzer *get_text_analyzer(void)
{
    return (IDWriteTextAnalyzer *)&textanalyzer;
}

static HRESULT WINAPI dwritenumbersubstitution_QueryInterface(IDWriteNumberSubstitution *iface, REFIID riid, void **obj)
{
    struct dwrite_numbersubstitution *This = impl_from_IDWriteNumberSubstitution(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IDWriteNumberSubstitution) ||
        IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IDWriteNumberSubstitution_AddRef(iface);
        return S_OK;
    }

    WARN("%s not implemented.\n", debugstr_guid(riid));

    *obj = NULL;

    return E_NOINTERFACE;
}

static ULONG WINAPI dwritenumbersubstitution_AddRef(IDWriteNumberSubstitution *iface)
{
    struct dwrite_numbersubstitution *This = impl_from_IDWriteNumberSubstitution(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p)->(%d)\n", This, ref);
    return ref;
}

static ULONG WINAPI dwritenumbersubstitution_Release(IDWriteNumberSubstitution *iface)
{
    struct dwrite_numbersubstitution *This = impl_from_IDWriteNumberSubstitution(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p)->(%d)\n", This, ref);

    if (!ref) {
        heap_free(This->locale);
        heap_free(This);
    }

    return ref;
}

static const struct IDWriteNumberSubstitutionVtbl numbersubstitutionvtbl = {
    dwritenumbersubstitution_QueryInterface,
    dwritenumbersubstitution_AddRef,
    dwritenumbersubstitution_Release
};

struct dwrite_numbersubstitution *unsafe_impl_from_IDWriteNumberSubstitution(IDWriteNumberSubstitution *iface)
{
    if (!iface || iface->lpVtbl != &numbersubstitutionvtbl)
        return NULL;
    return CONTAINING_RECORD(iface, struct dwrite_numbersubstitution, IDWriteNumberSubstitution_iface);
}

HRESULT create_numbersubstitution(DWRITE_NUMBER_SUBSTITUTION_METHOD method, const WCHAR *locale,
    BOOL ignore_user_override, IDWriteNumberSubstitution **ret)
{
    struct dwrite_numbersubstitution *substitution;

    *ret = NULL;

    if ((UINT32)method > DWRITE_NUMBER_SUBSTITUTION_METHOD_TRADITIONAL)
        return E_INVALIDARG;

    if (method != DWRITE_NUMBER_SUBSTITUTION_METHOD_NONE && !IsValidLocaleName(locale))
        return E_INVALIDARG;

    substitution = heap_alloc(sizeof(*substitution));
    if (!substitution)
        return E_OUTOFMEMORY;

    substitution->IDWriteNumberSubstitution_iface.lpVtbl = &numbersubstitutionvtbl;
    substitution->ref = 1;
    substitution->ignore_user_override = ignore_user_override;
    substitution->method = method;
    substitution->locale = heap_strdupW(locale);
    if (locale && !substitution->locale) {
        heap_free(substitution);
        return E_OUTOFMEMORY;
    }

    *ret = &substitution->IDWriteNumberSubstitution_iface;
    return S_OK;
}

/* IDWriteFontFallback */
static HRESULT WINAPI fontfallback_QueryInterface(IDWriteFontFallback *iface, REFIID riid, void **obj)
{
    struct dwrite_fontfallback *fallback = impl_from_IDWriteFontFallback(iface);

    TRACE("(%p)->(%s %p)\n", fallback, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IDWriteFontFallback) || IsEqualIID(riid, &IID_IUnknown)) {
        *obj = iface;
        IDWriteFontFallback_AddRef(iface);
        return S_OK;
    }

    WARN("%s not implemented.\n", debugstr_guid(riid));

    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI fontfallback_AddRef(IDWriteFontFallback *iface)
{
    struct dwrite_fontfallback *fallback = impl_from_IDWriteFontFallback(iface);
    TRACE("(%p)\n", fallback);
    return IDWriteFactory5_AddRef(fallback->factory);
}

static ULONG WINAPI fontfallback_Release(IDWriteFontFallback *iface)
{
    struct dwrite_fontfallback *fallback = impl_from_IDWriteFontFallback(iface);
    TRACE("(%p)\n", fallback);
    return IDWriteFactory5_Release(fallback->factory);
}

static int compare_mapping_range(const void *a, const void *b)
{
    UINT32 ch = *(UINT32 *)a;
    DWRITE_UNICODE_RANGE *range = (DWRITE_UNICODE_RANGE *)b;

    if (ch > range->last)
        return 1;
    else if (ch < range->first)
        return -1;
    else
        return 0;
}

static const struct fallback_mapping *find_fallback_mapping(struct dwrite_fontfallback *fallback, UINT32 ch)
{
    UINT32 i;

    for (i = 0; i < fallback->mappings_count; i++) {
        struct fallback_mapping *mapping = &fallback->mappings[i];

        if (bsearch(&ch, mapping->ranges, mapping->ranges_count, sizeof(*mapping->ranges),
                compare_mapping_range) != NULL)
            return mapping;
    }

    return NULL;
}

HRESULT create_matching_font(IDWriteFontCollection *collection, const WCHAR *name,
    DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch, IDWriteFont **font)
{
    IDWriteFontFamily *family;
    BOOL exists = FALSE;
    HRESULT hr;
    UINT32 i;

    *font = NULL;

    hr = IDWriteFontCollection_FindFamilyName(collection, name, &i, &exists);
    if (FAILED(hr))
        return hr;

    if (!exists)
        return E_FAIL;

    hr = IDWriteFontCollection_GetFontFamily(collection, i, &family);
    if (FAILED(hr))
        return hr;

    hr = IDWriteFontFamily_GetFirstMatchingFont(family, weight, stretch, style, font);
    IDWriteFontFamily_Release(family);
    return hr;
}

static HRESULT fallback_map_characters(IDWriteFont *font, const WCHAR *text, UINT32 length, UINT32 *mapped_length)
{
    HRESULT hr = S_OK;
    UINT32 i;

    for (i = 0; i < length; i++) {
        UINT16 script = get_char_script(text[i]);
        BOOL exists;

        if (script == Script_Unknown || script == Script_Common) {
            ++*mapped_length;
            continue;
        }

        /* stop on first unsupported character */
        exists = FALSE;
        hr = IDWriteFont_HasCharacter(font, text[i], &exists);
        if (hr == S_OK && exists)
            ++*mapped_length;
        else
            break;
    }

    return hr;
}

static HRESULT fallback_get_fallback_font(struct dwrite_fontfallback *fallback, const WCHAR *text, UINT32 length,
        DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch, UINT32 *mapped_length,
        IDWriteFont **mapped_font)
{
    const struct fallback_mapping *mapping;
    HRESULT hr;
    UINT32 i;

    *mapped_font = NULL;

    mapping = find_fallback_mapping(fallback, text[0]);
    if (!mapping) {
        WARN("No mapping range for %#x.\n", text[0]);
        return E_FAIL;
    }

    /* Now let's see what fallback can handle. Pick first font that could be created. */
    for (i = 0; i < mapping->families_count; i++) {
        hr = create_matching_font((IDWriteFontCollection *)fallback->systemcollection, mapping->families[i],
                weight, style, stretch, mapped_font);
        if (hr == S_OK) {
            TRACE("Created fallback font using family %s.\n", debugstr_w(mapping->families[i]));
            break;
        }
    }

    if (!*mapped_font) {
        WARN("Failed to create fallback font.\n");
        return E_FAIL;
    }

    hr = fallback_map_characters(*mapped_font, text, length, mapped_length);
    if (FAILED(hr))
        WARN("Mapping with fallback family %s failed, hr %#x.\n", debugstr_w(mapping->families[i]), hr);

    if (!*mapped_length) {
        IDWriteFont_Release(*mapped_font);
        *mapped_font = NULL;
    }

    return *mapped_length ? S_OK : E_FAIL;
}

static HRESULT WINAPI fontfallback_MapCharacters(IDWriteFontFallback *iface, IDWriteTextAnalysisSource *source,
    UINT32 position, UINT32 length, IDWriteFontCollection *basecollection, const WCHAR *basefamily,
    DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch, UINT32 *mapped_length,
    IDWriteFont **ret_font, FLOAT *scale)
{
    struct dwrite_fontfallback *fallback = impl_from_IDWriteFontFallback(iface);
    WCHAR *buff = NULL;
    const WCHAR *text;
    HRESULT hr;

    TRACE("(%p)->(%p %u %u %p, %s, %u, %u, %u, %p, %p, %p)\n", fallback, source, position, length,
        basecollection, debugstr_w(basefamily), weight, style, stretch, mapped_length, ret_font, scale);

    *mapped_length = 0;
    *scale = 1.0f;
    *ret_font = NULL;

    if (!source)
        return E_INVALIDARG;

    if (length == 0)
        return S_OK;

    if (!basecollection)
        basecollection = (IDWriteFontCollection*)fallback->systemcollection;

    hr = get_text_source_ptr(source, position, length, &text, &buff);
    if (FAILED(hr))
        goto done;

    if (basefamily && *basefamily) {
        hr = create_matching_font(basecollection, basefamily, weight, style, stretch, ret_font);
        if (FAILED(hr))
            goto done;

        hr = fallback_map_characters(*ret_font, text, length, mapped_length);
        if (FAILED(hr))
            goto done;
    }

    if (!*mapped_length) {
        IDWriteFont *mapped_font;

        hr = fallback_get_fallback_font(fallback, text, length, weight, style, stretch, mapped_length, &mapped_font);
        if (FAILED(hr)) {
            /* fallback wasn't found, keep base font if any, so we can get at least some visual output */
            if (*ret_font) {
                *mapped_length = length;
                hr = S_OK;
            }
        }
        else {
            if (*ret_font)
                IDWriteFont_Release(*ret_font);
            *ret_font = mapped_font;
        }
    }

done:
    heap_free(buff);
    return hr;
}

static const IDWriteFontFallbackVtbl fontfallbackvtbl = {
    fontfallback_QueryInterface,
    fontfallback_AddRef,
    fontfallback_Release,
    fontfallback_MapCharacters
};

HRESULT create_system_fontfallback(IDWriteFactory5 *factory, IDWriteFontFallback **ret)
{
    struct dwrite_fontfallback *fallback;

    *ret = NULL;

    fallback = heap_alloc(sizeof(*fallback));
    if (!fallback)
        return E_OUTOFMEMORY;

    fallback->IDWriteFontFallback_iface.lpVtbl = &fontfallbackvtbl;
    fallback->factory = factory;
    fallback->mappings = (struct fallback_mapping *)fontfallback_neutral_data;
    fallback->mappings_count = ARRAY_SIZE(fontfallback_neutral_data);
    IDWriteFactory5_GetSystemFontCollection(fallback->factory, FALSE, &fallback->systemcollection, FALSE);

    *ret = &fallback->IDWriteFontFallback_iface;
    return S_OK;
}

void release_system_fontfallback(IDWriteFontFallback *iface)
{
    struct dwrite_fontfallback *fallback = impl_from_IDWriteFontFallback(iface);
    IDWriteFontCollection1_Release(fallback->systemcollection);
    heap_free(fallback);
}

static ULONG WINAPI customfontfallback_AddRef(IDWriteFontFallback *iface)
{
    struct dwrite_fontfallback *fallback = impl_from_IDWriteFontFallback(iface);
    ULONG ref = InterlockedIncrement(&fallback->ref);
    TRACE("(%p)->(%d)\n", fallback, ref);
    return ref;
}

static ULONG WINAPI customfontfallback_Release(IDWriteFontFallback *iface)
{
    struct dwrite_fontfallback *fallback = impl_from_IDWriteFontFallback(iface);
    ULONG ref = InterlockedDecrement(&fallback->ref);

    TRACE("(%p)->(%d)\n", fallback, ref);

    if (!ref) {
        IDWriteFactory5_Release(fallback->factory);
        heap_free(fallback);
    }

    return ref;
}

static HRESULT WINAPI customfontfallback_MapCharacters(IDWriteFontFallback *iface, IDWriteTextAnalysisSource *source,
    UINT32 position, UINT32 length, IDWriteFontCollection *basecollection, const WCHAR *basefamily,
    DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch, UINT32 *mapped_length,
    IDWriteFont **ret_font, FLOAT *scale)
{
    struct dwrite_fontfallback *fallback = impl_from_IDWriteFontFallback(iface);

    FIXME("(%p)->(%p %u %u %p, %s, %u, %u, %u, %p, %p, %p)\n", fallback, source, position, length,
        basecollection, debugstr_w(basefamily), weight, style, stretch, mapped_length, ret_font, scale);

    return E_NOTIMPL;
}

static const IDWriteFontFallbackVtbl customfontfallbackvtbl =
{
    fontfallback_QueryInterface,
    customfontfallback_AddRef,
    customfontfallback_Release,
    customfontfallback_MapCharacters,
};

static HRESULT WINAPI fontfallbackbuilder_QueryInterface(IDWriteFontFallbackBuilder *iface, REFIID riid, void **obj)
{
    struct dwrite_fontfallback_builder *fallbackbuilder = impl_from_IDWriteFontFallbackBuilder(iface);

    TRACE("(%p)->(%s %p)\n", fallbackbuilder, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IDWriteFontFallbackBuilder) || IsEqualIID(riid, &IID_IUnknown)) {
        *obj = iface;
        IDWriteFontFallbackBuilder_AddRef(iface);
        return S_OK;
    }

    WARN("%s not implemented.\n", debugstr_guid(riid));

    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI fontfallbackbuilder_AddRef(IDWriteFontFallbackBuilder *iface)
{
    struct dwrite_fontfallback_builder *fallbackbuilder = impl_from_IDWriteFontFallbackBuilder(iface);
    ULONG ref = InterlockedIncrement(&fallbackbuilder->ref);
    TRACE("(%p)->(%d)\n", fallbackbuilder, ref);
    return ref;
}

static ULONG WINAPI fontfallbackbuilder_Release(IDWriteFontFallbackBuilder *iface)
{
    struct dwrite_fontfallback_builder *fallbackbuilder = impl_from_IDWriteFontFallbackBuilder(iface);
    ULONG ref = InterlockedDecrement(&fallbackbuilder->ref);

    TRACE("(%p)->(%d)\n", fallbackbuilder, ref);

    if (!ref) {
        UINT32 i;

        for (i = 0; i < fallbackbuilder->mappings_count; i++) {
            struct fallback_mapping *mapping = &fallbackbuilder->mappings[i];
            UINT32 j;

            for (j = 0; j < mapping->families_count; j++)
                heap_free(mapping->families[j]);
            heap_free(mapping->families);

            if (mapping->collection)
                IDWriteFontCollection_Release(mapping->collection);
            heap_free(mapping->ranges);
            heap_free(mapping->locale);
        }

        IDWriteFactory5_Release(fallbackbuilder->factory);
        heap_free(fallbackbuilder->mappings);
        heap_free(fallbackbuilder);
    }

    return ref;
}

static HRESULT WINAPI fontfallbackbuilder_AddMapping(IDWriteFontFallbackBuilder *iface,
        const DWRITE_UNICODE_RANGE *ranges, UINT32 ranges_count, WCHAR const **target_families, UINT32 families_count,
        IDWriteFontCollection *collection, WCHAR const *locale, WCHAR const *base_family, FLOAT scale)
{
    struct dwrite_fontfallback_builder *fallbackbuilder = impl_from_IDWriteFontFallbackBuilder(iface);
    struct fallback_mapping *mapping;
    UINT32 i;

    TRACE("(%p)->(%p, %u, %p, %u, %p, %s, %s, %f)\n", fallbackbuilder, ranges, ranges_count, target_families,
            families_count, collection, debugstr_w(locale), debugstr_w(base_family), scale);

    if (!ranges || ranges_count == 0 || !target_families || families_count == 0 || scale < 0.0f)
        return E_INVALIDARG;

    if (base_family)
        FIXME("base family ignored.\n");

    if (fallbackbuilder->mappings_count == fallbackbuilder->mappings_capacity) {
        struct fallback_mapping *mappings;

        if (fallbackbuilder->mappings_capacity == 0) {
            if ((mappings = heap_calloc(16, sizeof(*mappings))))
                fallbackbuilder->mappings_capacity = 16;
        }
        else {
            if ((mappings = heap_realloc(fallbackbuilder->mappings, sizeof(*fallbackbuilder->mappings) *
                    fallbackbuilder->mappings_capacity * 2)))
                fallbackbuilder->mappings_capacity *= 2;
        }
        if (!mappings)
            return E_OUTOFMEMORY;

        fallbackbuilder->mappings = mappings;
    }

    mapping = &fallbackbuilder->mappings[fallbackbuilder->mappings_count++];

    mapping->ranges = heap_calloc(ranges_count, sizeof(*mapping->ranges));
    memcpy(mapping->ranges, ranges, sizeof(*mapping->ranges) * ranges_count);
    mapping->ranges_count = ranges_count;
    mapping->families = heap_alloc_zero(sizeof(*mapping->families) * families_count);
    mapping->families_count = families_count;
    for (i = 0; i < families_count; i++)
        mapping->families[i] = heap_strdupW(target_families[i]);
    mapping->collection = collection;
    if (mapping->collection)
        IDWriteFontCollection_AddRef(mapping->collection);
    mapping->locale = heap_strdupW(locale);
    mapping->scale = scale;

    return S_OK;
}

static HRESULT WINAPI fontfallbackbuilder_AddMappings(IDWriteFontFallbackBuilder *iface, IDWriteFontFallback *fallback)
{
    struct dwrite_fontfallback_builder *fallbackbuilder = impl_from_IDWriteFontFallbackBuilder(iface);

    FIXME("(%p)->(%p): stub\n", fallbackbuilder, fallback);

    return E_NOTIMPL;
}

static HRESULT WINAPI fontfallbackbuilder_CreateFontFallback(IDWriteFontFallbackBuilder *iface,
        IDWriteFontFallback **ret)
{
    struct dwrite_fontfallback_builder *fallbackbuilder = impl_from_IDWriteFontFallbackBuilder(iface);
    struct dwrite_fontfallback *fallback;

    FIXME("(%p)->(%p): stub\n", fallbackbuilder, ret);

    *ret = NULL;

    fallback = heap_alloc(sizeof(*fallback));
    if (!fallback)
        return E_OUTOFMEMORY;

    fallback->IDWriteFontFallback_iface.lpVtbl = &customfontfallbackvtbl;
    fallback->ref = 1;
    fallback->factory = fallbackbuilder->factory;
    IDWriteFactory5_AddRef(fallback->factory);

    *ret = &fallback->IDWriteFontFallback_iface;
    return S_OK;
}

static const IDWriteFontFallbackBuilderVtbl fontfallbackbuildervtbl =
{
    fontfallbackbuilder_QueryInterface,
    fontfallbackbuilder_AddRef,
    fontfallbackbuilder_Release,
    fontfallbackbuilder_AddMapping,
    fontfallbackbuilder_AddMappings,
    fontfallbackbuilder_CreateFontFallback,
};

HRESULT create_fontfallback_builder(IDWriteFactory5 *factory, IDWriteFontFallbackBuilder **ret)
{
    struct dwrite_fontfallback_builder *builder;

    *ret = NULL;

    builder = heap_alloc_zero(sizeof(*builder));
    if (!builder)
        return E_OUTOFMEMORY;

    builder->IDWriteFontFallbackBuilder_iface.lpVtbl = &fontfallbackbuildervtbl;
    builder->ref = 1;
    builder->factory = factory;
    IDWriteFactory5_AddRef(builder->factory);

    *ret = &builder->IDWriteFontFallbackBuilder_iface;
    return S_OK;
}
