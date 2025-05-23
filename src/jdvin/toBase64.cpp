#include "toBase64.h"

void convertToBase64(std::vector<uint8_t>& data_vec) {
    static constexpr uint8_t base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    uint32_t input_size = static_cast<uint32_t>(data_vec.size());
    uint32_t output_size = ((input_size + 2) / 3) * 4; 

    std::vector<uint8_t> temp_vec(output_size); 

    uint32_t j = 0;
    for (uint32_t i = 0; i < input_size; i += 3) {
        uint32_t octet_a = data_vec[i];
        uint32_t octet_b = (i + 1 < input_size) ? data_vec[i + 1] : 0;
        uint32_t octet_c = (i + 2 < input_size) ? data_vec[i + 2] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        temp_vec[j++] = base64_table[(triple >> 18) & 0x3F];
        temp_vec[j++] = base64_table[(triple >> 12) & 0x3F];
        temp_vec[j++] = (i + 1 < input_size) ? base64_table[(triple >> 6) & 0x3F] : '=';
        temp_vec[j++] = (i + 2 < input_size) ? base64_table[triple & 0x3F] : '=';
    }
    data_vec.swap(temp_vec);
    std::vector<uint8_t>().swap(temp_vec);
}
