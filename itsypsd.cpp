/*
The MIT License (MIT)

Copyright (c) 2015 Cameron Buschardt

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

Adobe Photoshop PSD Reference
http://www.adobe.com/devnet-apps/photoshop/fileformatashtml/#50577409_16000

*/

#define _CRT_SECURE_NO_WARNINGS
#include "itsypsd.h"

#include <vector>
#include <string>
#include <stdint.h>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <assert.h>
using namespace std;
using namespace std::tr2::sys;

uint32_t read32(char * & i, char * e) {
    uint32_t value = 0;
    if (i + 4 > e || i + 4 < i)
        throw "Truncated PSD";
    value = (value << 8) | (uint8_t)*i++;
    value = (value << 8) | (uint8_t)*i++;
    value = (value << 8) | (uint8_t)*i++;
    value = (value << 8) | (uint8_t)*i++;
    return value;
}

uint32_t read16(char * & i, char * e) {
    uint32_t value = 0;
    if (i + 2 > e || i + 2 < i)
        throw "Truncated PSD";
    value = (value << 8) | (uint8_t)*i++;
    value = (value << 8) | (uint8_t)*i++;
    return value;
}

uint32_t read8(char * & i, char * e) {
    if (i + 1 > e)
        throw "Truncated PSD";
    return (uint8_t)*i++;
}

void skip(char * & i, size_t amt, char * e) {
    if (i + amt > e || i + amt < i)
        throw "Truncated PSD";
    i += amt;
}

bool psd::load(const string & filename) {
    std::vector<char> v;
    if (FILE *fp = fopen(filename.c_str(), "rb")) { // Load bitmap
        char buf[1024];
        while (size_t len = fread(buf, 1, sizeof(buf), fp))
            v.insert(v.end(), buf, buf + len);
        fclose(fp);
    }
    char * offset = &v[0];
    char * e = offset + v.size();
    
    // File header
    if (read32(offset, e) != '8BPS')  {
        cerr << "psd::load " << filename << " -- Signature not found." << endl;
        return false;
    }
    if (read16(offset, e) != 1) {        // Version 
        cerr << "psd::load " << filename << " -- Version not found." << endl;
        return false;
    }

    skip(offset,6, e);                  // Zeros
    auto channels = read16(offset,e);
    height = read32(offset,e);
    width = read32(offset,e);
    if (read16(offset, e) != 8) {        // Depth must be 8bits per compononent
        cerr << "psd::load " << filename << " -- Invalid color depth.  Open image menu in photoshop.  Expand mode.  Select 8 bits/channel." << endl;
        return false;
    }
    if (read16(offset, e) != 3) {        // Color mode must be RGB
        cerr << "psd::load " << filename << " -- Invalid color mode.  Open image menu in photoshop.  Expand mode.  Select RGB." << endl;
        return false;
    }

    // Color mode data (Don't care since RGB)
    auto colorDataLength = read32(offset,e);
    skip(offset, colorDataLength, e);

    // Image resources (Don't care)
    auto imageResourceLength = read32(offset,e);
    skip(offset, imageResourceLength, e);

    struct layer {
        uint32_t top, left, bot, right;
        string name;
        bool is_group;
        struct channel {
            int kind; // 0 = red, 1 = green, etc.  -1 = trans mask, -2 = user mask, -3 = user+vector mask
            vector<uint8_t> data;
        };
        vector<channel> channels;
    };
    vector<layer> raw_layers;

    // Layer and Masks
    auto layerMaskSection = read32(offset,e);
    auto nextOffset = offset + layerMaskSection;
    {
        auto layersLength = read32(offset,e);
        auto count = (int16_t)read16(offset,e);
        if (count < 0)
            count = -count;     // Signals that first alpha layer is merged alpha.  We don't care since we don't look at merged result.
        raw_layers.resize(count);

        // Layer record
        for (int i = 0; i < count; i++) {
            raw_layers[i].top = read32(offset,e);
            raw_layers[i].left = read32(offset,e);
            raw_layers[i].bot = read32(offset,e);
            raw_layers[i].right = read32(offset,e);
            raw_layers[i].channels.resize(read16(offset,e));
            for (uint32_t ch = 0; ch < raw_layers[i].channels.size(); ch++) {
                raw_layers[i].channels[ch].kind = (int16_t)read16(offset,e); 
                uint32_t length = read32(offset,e);
            }

            auto sig = read32(offset,e);
            if (sig != '8BIM') {
                cerr << "psd::load " << filename << " -- Invalid signature on layer" << endl;
                return false;
            }

            auto blend = read32(offset,e); // 'pass', 'norm', etc.
            auto opacity = read8(offset,e);
            auto clipping = read8(offset, e);
            auto flags = read8(offset, e);
            read8(offset, e); // zero

            // Skip extra data
            auto len_extradata = read32(offset,e);
            auto end_extradata = offset + len_extradata;
            
            // Layer mask data
            auto layer_masklength = read32(offset,e);
            skip(offset, layer_masklength, e);

            // Layer blending ranges
            auto len_blendingranges = read32(offset,e);
            skip(offset, len_blendingranges, e);

            // Layer name
            {
                auto len = 1 + read8(offset, e);
                for (uint32_t j = 1; j < len; j++)
                    raw_layers[i].name += read8(offset, e);
                while (len & 3) 
                    read8(offset, e), len++;
            }

            // Pixel data irrelevent to appearance of doc.  Probably a group!
            raw_layers[i].is_group = (flags & 0x18) == 0x18;

            // '8BIM' headers with code/len.  Such as 'SOCO'.  This is all adjustment raw_layers & friends
            skip(offset, end_extradata - offset, e);
        }

        // Channel image data?
        for (uint32_t i = 0; i < raw_layers.size(); i++) {
            auto height = raw_layers[i].bot - raw_layers[i].top;
            auto width = raw_layers[i].right - raw_layers[i].left;

            for (uint32_t ch = 0; ch < raw_layers[i].channels.size(); ch++) {
                auto comp = read16(offset,e);
                switch (comp)
                {
                    case 0: // RAW
                        std::copy(offset, offset + width * height, back_inserter(raw_layers[i].channels[ch].data));
                        offset += width * height;
                        break;

                    case 1: // RLE
                    {
                        vector<size_t> length;
                        offset += 2 * height; // Skip jump table

                        while (raw_layers[i].channels[ch].data.size() < width * height) {
                            uint8_t len = read8(offset, e);
                            if (len < 0x80) {
                                len++;
                                while (len--)
                                    raw_layers[i].channels[ch].data.push_back(read8(offset, e));
                            }
                            else if (len > 0x80) {
                                len = 1 - len;
                                uint8_t repeated = read8(offset, e);
                                while (len--)
                                    raw_layers[i].channels[ch].data.push_back(repeated);
                            }
                        }
                        break;
                    }
                    default:
                        cerr << "psd::load " << filename << " -- Unsupported compression kind.  Open Edit Menu. Select Preferences. Select File handling. Try 'Maximize PSD compatibility -> Always' or 'Disable compression of PSD/PSB files' " << endl;
                        assert(0);
                }
            }
        }
    }

    // Combine channels and pad all raw_layers to match image width/height
    vector<string> location;
    for (int layer = raw_layers.size() - 1; layer >= 0; layer--) {
        if (raw_layers[layer].is_group) {
            if (raw_layers[layer].name == "</Layer group>")
                location.pop_back();
            else
                location.push_back(raw_layers[layer].name);
            continue;
        }

        layers.push_back(make_shared<psd::layer>());
        layers.back()->name = location;
        layers.back()->name.push_back(raw_layers[layer].name);

        layers.back()->width = width, layers.back()->height = height;
        vector<psd::pixel> & pixels = layers.back()->pixels;
        pixels.resize(width * height, 0);
        
        for (uint32_t ch = 0; ch < raw_layers[layer].channels.size(); ch++) {
            uint32_t x = raw_layers[layer].left;
            uint32_t y = raw_layers[layer].bot - 1;
            uint32_t i = 0;
            auto kind = raw_layers[layer].channels[ch].kind;
            if (kind >= 0 && kind <= 2)
                ;
            else if (kind == -1)
                kind = 3;           // Standard alpha channel
            else {
                cerr << "psd::load " << filename << " -- Unsupported transparency channel kind.  Ignoring. " << endl;
                continue;
            }
            kind *= 8;

            while (i < raw_layers[layer].channels[ch].data.size()) {
                int color = raw_layers[layer].channels[ch].data[i++];
                if (x < width && y < height)                    // Layer might be larger than image?!
                    pixels[x + y * width] |= color << kind;
                x++;
                if (x >= raw_layers[layer].right)
                    x = raw_layers[layer].left, y--;
            }            
        }
    }

    // Image Data
    return true;
}
