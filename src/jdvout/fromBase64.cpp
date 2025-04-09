void convertFromBase64(std::vector<uint8_t>& data_vec) {
    static constexpr int8_t base64_decode_table[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, 
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, 
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, 
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    };

    uint32_t input_size = static_cast<uint32_t>(data_vec.size());
    if (input_size == 0 || input_size % 4 != 0) {
        throw std::invalid_argument("Base64 input size must be a multiple of 4 and non-empty");
    }

    uint32_t padding_count = 0;
    if (data_vec[input_size - 1] == '=') padding_count++;
    if (data_vec[input_size - 2] == '=') padding_count++;
    for (uint32_t i = 0; i < input_size - padding_count; i++) {
        if (data_vec[i] == '=') {
            throw std::invalid_argument("Invalid '=' character in Base64 input");
        }
    }

    uint32_t output_size = (input_size / 4) * 3 - padding_count;
    std::vector<uint8_t> temp_vec;
    temp_vec.reserve(output_size);

    for (uint32_t i = 0; i < input_size; i += 4) {
        int sextet_a = base64_decode_table[data_vec[i]];
        int sextet_b = base64_decode_table[data_vec[i + 1]];
        int sextet_c = base64_decode_table[data_vec[i + 2]];
        int sextet_d = base64_decode_table[data_vec[i + 3]];

        if (sextet_a == -1 || sextet_b == -1 ||
            (sextet_c == -1 && data_vec[i + 2] != '=') ||
            (sextet_d == -1 && data_vec[i + 3] != '=')) {
            throw std::invalid_argument("Invalid Base64 character encountered");
        }

        uint32_t triple = (sextet_a << 18) | (sextet_b << 12) | ((sextet_c & 0x3F) << 6) | (sextet_d & 0x3F);
        
        temp_vec.emplace_back((triple >> 16) & 0xFF);
        if (data_vec[i + 2] != '=') temp_vec.emplace_back((triple >> 8) & 0xFF);
        if (data_vec[i + 3] != '=') temp_vec.emplace_back(triple & 0xFF);
    }
    data_vec.swap(temp_vec);
    std::vector<uint8_t>().swap(temp_vec);
}