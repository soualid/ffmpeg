// https://tech.ebu.ch/docs/tech/tech3264.pdf

#include <stdbool.h>
#include <libavformat/internal.h>
#include "ass_split.h"
#include "libavcodec/ass.h"
#include "avcodec.h"
#include "codec.h"
#include "codec_internal.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavutil/intreadwrite.h"

#define TTI_TEXT_FIELD_OFFSET 16
#define TTI_TEXT_FIELD_LENGTH 112
#define TTI_BLOCK_SIZE 128


typedef struct {
    AVSubtitleRect **rects;
    int64_t end_timecode;
} EbuStlContext;

static av_cold int ebustl_decode_init(AVCodecContext *avctx)
{
    av_log(avctx, AV_LOG_DEBUG, "Initializing EBU STL decoder\n");
    EbuStlContext *ctx = avctx->priv_data;
    ctx->rects = NULL;
    if (avctx->time_base.num == 0 || avctx->time_base.den == 0) {
        avctx->time_base = (AVRational){1, 1000};
    }
    if (avctx->width <= 0 || avctx->height <= 0) {
        av_log(avctx, AV_LOG_WARNING, "Video dimensions not set in AVCodecContext, setting defaults.\n");
        avctx->width = 720;
        avctx->height = 576;
    }
    const char *header_data =
        "[Script Info]\n"
        "; Script generated by FFmpeg\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 720\n"
        "PlayResY: 576\n"
        "ScaledBorderAndShadow: yes\n"
        "YCbCr Matrix: None\n\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,30,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,1,1,2,10,10,10,1\n\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    int header_size = strlen(header_data) + 1;
    avctx->subtitle_header = av_mallocz(header_size);
    if (!avctx->subtitle_header) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate subtitle header buffer\n");
        return AVERROR(ENOMEM);
    }
    memcpy(avctx->subtitle_header, header_data, header_size - 1);
    avctx->subtitle_header[header_size - 1] = '\0';
    avctx->subtitle_header_size = header_size;
    av_log(avctx, AV_LOG_DEBUG, "Subtitle header initialized successfully\n");
    return 0;
}

// Log the hex content of the TTI block for debugging
static void log_tti_block_hex(const uint8_t *buf, int buf_size) {
    char hex_output[TTI_BLOCK_SIZE * 3 + 1];
    int i;
    for (i = 0; i < TTI_BLOCK_SIZE && i < buf_size; i++) {
        snprintf(hex_output + i * 3, 4, "%02X ", buf[i]);
    }
    av_log(NULL, AV_LOG_DEBUG, "TTI Block (hex): %s\n", hex_output);
}

static const char* map_iso6937_to_utf8(unsigned char diacritic, char base);

static const char* map_iso6937_to_utf8(unsigned char diacritic, char base) {
    switch (diacritic) {
        case 0xC1:  // Accent grave
            switch (base) {
                case 'A': return "À";
                case 'E': return "È";
                case 'I': return "Ì";
                case 'O': return "Ò";
                case 'U': return "Ù";
                case 'a': return "à";
                case 'e': return "è";
                case 'i': return "ì";
                case 'o': return "ò";
                case 'u': return "ù";
            }
            break;
        case 0xC2:  // Accent aigu
            switch (base) {
                case 'A': return "Á";
                case 'E': return "É";
                case 'I': return "Í";
                case 'O': return "Ó";
                case 'U': return "Ú";
                case 'a': return "á";
                case 'e': return "é";  // C'est ici que la conversion se fait pour 'é'
                case 'i': return "í";
                case 'o': return "ó";
                case 'u': return "ú";
            }
            break;
        case 0xC3:  // Accent circonflexe
            switch (base) {
                case 'A': return "Â";
                case 'E': return "Ê";
                case 'I': return "Î";
                case 'O': return "Ô";
                case 'U': return "Û";
                case 'a': return "â";
                case 'e': return "ê";
                case 'i': return "î";
                case 'o': return "ô";
                case 'u': return "û";
            }
            break;

        case 0xC8:  // Tréma
            switch (base) {
                case 'A': return "Ä";
                case 'E': return "Ë";
                case 'I': return "Ï";
                case 'O': return "Ö";
                case 'U': return "Ü";
                case 'a': return "ä";
                case 'e': return "ë";
                case 'i': return "ï";
                case 'o': return "ö";
                case 'u': return "ü";
            }
            break;            
        // Ajoute d'autres cas si nécessaire
    }
    return NULL;  // Retourne NULL si aucun mappage n'est trouvé
}

static char* convert_iso6937_to_utf8(const unsigned char *input, size_t length) {
    size_t out_len = length * 4;  // UTF-8 peut prendre jusqu'à 4 octets par caractère
    char *output = malloc(out_len);
    if (!output) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char diacritic = input[i];

        // Vérifie si c'est un diacritique
        if (diacritic >= 0xC1 && diacritic <= 0xCF) {
            if (i + 1 < length) {
                const char* utf8_char = map_iso6937_to_utf8(diacritic, input[i + 1]);
                if (utf8_char) {
                    size_t utf8_len = strlen(utf8_char);
                    memcpy(&output[j], utf8_char, utf8_len);
                    j += utf8_len;
                    i++;  // Passe au caractère suivant
                    continue;
                }
            }
        }

        // Si ce n'est pas un diacritique, copie simplement le caractère
        output[j++] = input[i];
    }

    output[j] = '\0';  // Terminaison de la chaîne
    return output;
}

#include "libavcodec/ass.h"

// Extract text and colors from TTI block
bool extract_colors_from_tti(const uint8_t *buf, uint8_t *text_color, uint8_t *background_color, int line_count);
bool extract_colors_from_tti(const uint8_t *buf, uint8_t *text_color, uint8_t *background_color, int line_count) {
    *text_color = 0x00;  // Default to white
    *background_color = 0x07;  // Default to black
    int current_line = 0;  // Start at the first line

    for (int i = TTI_TEXT_FIELD_OFFSET; i < TTI_TEXT_FIELD_OFFSET + TTI_TEXT_FIELD_LENGTH; ++i) {
        uint8_t byte = buf[i];

        // If we encounter a 0x8A byte, we move to the next line
        if (byte == 0x8A) {
            current_line++;
            av_log(NULL, AV_LOG_INFO, "New line detected at offset %d (line %d)\n", i, current_line);

            // If we've reached the desired line, we stop
            if (current_line > line_count) {
                av_log(NULL, AV_LOG_INFO, "Line %d detected, stopping color search\n", line_count);
                break;
            }

            // Reset colors for the new line
            *text_color = 0x00;
            *background_color = 0x07;
        }

        // Only apply colors if we're on the correct line
        if (current_line == line_count) {
            if (byte >= 0x00 && byte <= 0x07) {
                *text_color = byte;

                // Log the text color found and its offset
                av_log(NULL, AV_LOG_INFO, "Text color found at offset %d: 0x%02X (%s)\n", i, byte,
                       byte == 0x00 ? "White" :
                       byte == 0x01 ? "Red" :
                       byte == 0x02 ? "Green" :
                       byte == 0x03 ? "Yellow" :
                       byte == 0x04 ? "Blue" :
                       byte == 0x05 ? "Magenta" :
                       byte == 0x06 ? "Cyan" :
                       byte == 0x07 ? "Black" : "Unknown");
            } else if (byte >= 0x10 && byte <= 0x17) {
                *background_color = byte & 0x07;

                // Log the background color found and its offset
                av_log(NULL, AV_LOG_INFO, "Background color found at offset %d: 0x%02X (%s)\n", i, byte,
                       (*background_color == 0x00) ? "White" :
                       (*background_color == 0x01) ? "Yellow" :
                       (*background_color == 0x02) ? "Green" :
                       (*background_color == 0x03) ? "Blue" :
                       (*background_color == 0x04) ? "Red" :
                       (*background_color == 0x05) ? "Magenta" :
                       (*background_color == 0x06) ? "Cyan" :
                       (*background_color == 0x07) ? "Black" : "Unknown");
            }
        }

    }

    return true;
}

// Function to extract text and colors from TTI block and return an ASS formatted string
// Function to extract text and colors from TTI block and return an ASS formatted string
static char *extract_text_and_colors_from_tti_block(const uint8_t *tti_block, int tti_block_size) {
    int text_field_offset = 13;
    char *ass_string = NULL;
    int i, text_len = 0;
    char *line = malloc(tti_block_size * 2 * sizeof(char));
    int last_was_newline = 0;
    int line_count = 0;  // Line counter

    uint8_t text_color, background_color;
    extract_colors_from_tti(tti_block, &text_color, &background_color, line_count);

    char *text_color_str = NULL;
    if (text_color == 0) {
        text_color_str = "{\\c&HFFFFFF&}"; // White
    } else if (text_color == 1) {
        text_color_str = "{\\c&H0000FF&}"; // Red (BGR: 0000FF)
    } else if (text_color == 2) {
        text_color_str = "{\\c&H00FF00&}"; // Green (BGR: 00FF00)
    } else if (text_color == 3) {
        text_color_str = "{\\c&H00FFFF&}"; // Yellow (BGR: 00FFFF)
    } else if (text_color == 4) {
        text_color_str = "{\\c&HFF0000&}"; // Blue (BGR: FF0000)
    } else if (text_color == 5) {
        text_color_str = "{\\c&HFF00FF&}"; // Magenta (BGR: FF00FF)
    } else if (text_color == 6) {
        text_color_str = "{\\c&HFFFF00&}"; // Cyan (BGR: FFFF00)
    } else if (text_color == 7) {
        text_color_str = "{\\c&H000000&}"; // Black
    }

    char *border_color = NULL;
    if (background_color == 0) {
        border_color = "{\\3c&HFFFFFF&}"; // White
    } else if (background_color == 1) {
        border_color = "{\\3c&H00FFFF&}"; // Yellow (BGR: 00FFFF)
    } else if (background_color == 2) {
        border_color = "{\\3c&H00FF00&}"; // Green (BGR: 00FF00)
    } else if (background_color == 3) {
        border_color = "{\\3c&HFF0000&}"; // Blue (BGR: FF0000)
    } else if (background_color == 4) {
        border_color = "{\\3c&H0000FF&}"; // Red (BGR: 0000FF)
    } else if (background_color == 5) {
        border_color = "{\\3c&HFF00FF&}"; // Magenta (BGR: FF00FF)
    } else if (background_color == 6) {
        border_color = "{\\3c&HFFFF00&}"; // Cyan (BGR: FFFF00)
    } else if (background_color == 7) {
        border_color = "{\\3c&H000000&}"; // Black
    }

    ass_string = av_asprintf("%s%s", text_color_str, border_color); // Initialize with color

    for (i = text_field_offset; i < tti_block_size; i++) {
        uint8_t character = tti_block[i];

        if (character == 0x8A) { // New line
            line[text_len] = '\0'; // Terminate the line

            // Convert line to UTF-8
            char *utf8_line = convert_iso6937_to_utf8((unsigned char*)line, text_len);

            if (utf8_line) {
                if (line_count > 0 && text_len > 0) {
                    ass_string = av_asprintf("%s\\N%s", ass_string, utf8_line); // Add newline before the new line if it is not empty
                } else if (text_len > 0) {
                    ass_string = av_asprintf("%s%s", ass_string, utf8_line); // Add first line without newline if it is not empty
                }
                av_free(utf8_line);  // Libérer la mémoire après utilisation
            } else {
                av_log(NULL, AV_LOG_ERROR, "Erreur lors de la conversion ISO 6937 vers UTF-8\n");
            }

            line_count++; // Increment line counter
            text_len = 0; // Reset the counter for the next line

            // Extract colors for the next line
            extract_colors_from_tti(tti_block, &text_color, &background_color, line_count);

            if (text_color == 0) {
                text_color_str = "{\\c&HFFFFFF&}"; // White
            } else if (text_color == 1) {
                text_color_str = "{\\c&HFF0000&}"; // Blue (BGR: FF0000)
            } else if (text_color == 2) {
                text_color_str = "{\\c&H00FF00&}"; // Green (BGR: 00FF00)
            } else if (text_color == 3) {
                text_color_str = "{\\c&H00FFFF&}"; // Yellow (BGR: 00FFFF)
            } else if (text_color == 4) {
                text_color_str = "{\\c&H0000FF&}"; // Red (BGR: 0000FF)
            } else if (text_color == 5) {
                text_color_str = "{\\c&HFF00FF&}"; // Magenta (BGR: FF00FF)
            } else if (text_color == 6) {
                text_color_str = "{\\c&HFFFF00&}"; // Cyan (BGR: FFFF00)
            } else if (text_color == 7) {
                text_color_str = "{\\c&H000000&}"; // Black
            }

            if (background_color == 0) {
                border_color = "{\\3c&HFFFFFF&}"; // White
            } else if (background_color == 1) {
                border_color = "{\\3c&H00FFFF&}"; // Yellow (BGR: 00FFFF)
            } else if (background_color == 2) {
                border_color = "{\\3c&H00FF00&}"; // Green (BGR: 00FF00)
            } else if (background_color == 3) {
                border_color = "{\\3c&HFF0000&}"; // Blue (BGR: FF0000)
            } else if (background_color == 4) {
                border_color = "{\\3c&H0000FF&}"; // Red (BGR: 0000FF)
            } else if (background_color == 5) {
                border_color = "{\\3c&HFF00FF&}"; // Magenta (BGR: FF00FF)
            } else if (background_color == 6) {
                border_color = "{\\3c&HFFFF00&}"; // Cyan (BGR: FFFF00)
            } else if (background_color == 7) {
                border_color = "{\\3c&H000000&}"; // Black
            }

            ass_string = av_asprintf("%s%s%s", ass_string, text_color_str, border_color); // Add the new color
            continue;
        }

        if (character == 0x8F) {
            break; // End of text
        }

        if (character >= 32) {
            line[text_len++] = character; // Add characters to the line
        }
    }

    line[text_len] = '\0'; // Terminate the last line

    // Convert the final line to UTF-8
    char *utf8_line = convert_iso6937_to_utf8((unsigned char*)line, text_len);
    if (utf8_line) {
        if (line_count > 0 && text_len > 0) {
            ass_string = av_asprintf("%s\\N%s", ass_string, utf8_line); // Add newline before the last line if it is not empty
        } else if (text_len > 0) {
            ass_string = av_asprintf("%s%s", ass_string, utf8_line); // Add the first line without newline if it is not empty
        }
        av_free(utf8_line);  // Libérer la mémoire après utilisation
    } else {
        av_log(NULL, AV_LOG_ERROR, "Erreur lors de la conversion ISO 6937 vers UTF-8\n");
    }

    return ass_string;
}


static int ebustl_decode_frame(AVCodecContext *avctx, AVSubtitle *sub,
                               int *got_sub_ptr, const AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    FFASSDecoderContext *s = avctx->priv_data;
    *got_sub_ptr = 0;

    while (buf_size >= TTI_BLOCK_SIZE) {
        // Extract text and colors from the TTI block
        log_tti_block_hex(buf, TTI_BLOCK_SIZE);
        char *ass_text = extract_text_and_colors_from_tti_block(buf, TTI_BLOCK_SIZE);
        av_log(NULL, AV_LOG_DEBUG, "Extracted ASS text: %s\n", ass_text);

        // Ignore empty subtitles
        if (!ass_text || ass_text[0] == '\0') {
            av_freep(&ass_text);
            buf += TTI_BLOCK_SIZE;
            buf_size -= TTI_BLOCK_SIZE;
            continue;
        }

        // Handle text alignment
        uint8_t justification_code = buf[14];
        uint8_t vertical_position = buf[13];
        char *alignment_str = NULL;
        int horizontal_alignment = 2;  // Default to center alignment
        if (justification_code == 0x01) {
            horizontal_alignment = 1;  // Left alignment
        } else if (justification_code == 0x02) {
            horizontal_alignment = 2;  // Center alignment
        } else if (justification_code == 0x03) {
            horizontal_alignment = 3;  // Right alignment
        }

        int vertical_alignment = 2;  // Default to middle alignment
        if (vertical_position < 8) {
            vertical_alignment = 3;  // Bottom alignment
        } else if (vertical_position >= 8 && vertical_position <= 16) {
            vertical_alignment = 2;  // Middle alignment
        } else {
            vertical_alignment = 1;  // Top alignment
        }

        alignment_str = av_asprintf("{\\an%d}", (vertical_alignment - 1) * 3 + horizontal_alignment);

        // Build the final ASS string with color and alignment
        char *final_ass_text = av_asprintf("%s%s%s", alignment_str, ass_text, "{\\bord3}");
        av_log(NULL, AV_LOG_DEBUG, "final_ass_text: %s\n", final_ass_text);

        // Add the subtitle to the ASS structure
        //sub->start_display_time = av_rescale_q(0, (AVRational){1, 1000}, avctx->time_base);
        sub->end_display_time = av_rescale_q(avpkt->duration, (AVRational){1, 1000}, avctx->time_base);

        ff_ass_add_rect(sub, final_ass_text, s->readorder++, 0, NULL, NULL);

        av_freep(&ass_text);
        av_freep(&final_ass_text);
        av_freep(&alignment_str);

        buf += TTI_BLOCK_SIZE;
        buf_size -= TTI_BLOCK_SIZE;
        *got_sub_ptr = 1;
    }

    return 0;
}

static av_cold int ebustl_decode_close(AVCodecContext *avctx)
{
    EbuStlContext *ctx = avctx->priv_data;
    return 0;
}

const FFCodec ff_ebustl_decoder = {
    .p.name           = "ebustl",
    CODEC_LONG_NAME("EBU STL Subtitle"),
    .p.type           = AVMEDIA_TYPE_SUBTITLE,
    .p.id             = AV_CODEC_ID_EBUSTL,
    .init             = ebustl_decode_init,
    FF_CODEC_DECODE_SUB_CB(ebustl_decode_frame),
    .close            = ebustl_decode_close,
    .priv_data_size   = sizeof(EbuStlContext),
};
