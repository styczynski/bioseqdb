template<bool WithValues, size_t Count, bool AssumeDistinct = false> void TinySortAVX(
    const int *inKeys, const int *inVals,
    int *dstKeys, int *dstVals
) {
    static_assert(Count % 8 == 0, "TinySort: unaligned size");
    for (size_t i = 0; i < Count; i += 4) {

        __m128i reg = _mm_load_si128((__m128i*)&inKeys[i]);
        __m128i reg0 = _mm_shuffle_epi32(reg, SHUF(0, 0, 0, 0));
        __m128i reg1 = _mm_shuffle_epi32(reg, SHUF(1, 1, 1, 1));
        __m128i reg2 = _mm_shuffle_epi32(reg, SHUF(2, 2, 2, 2));
        __m128i reg3 = _mm_shuffle_epi32(reg, SHUF(3, 3, 3, 3));
        __m256i regg0 = _mm256_set_m128i(reg0, reg0);
        __m256i regg1 = _mm256_set_m128i(reg1, reg1);
        __m256i regg2 = _mm256_set_m128i(reg2, reg2);
        __m256i regg3 = _mm256_set_m128i(reg3, reg3);

        __m256i cnt0 = _mm256_setzero_si256();
        __m256i cnt1 = _mm256_setzero_si256();
        __m256i cnt2 = _mm256_setzero_si256();
        __m256i cnt3 = _mm256_setzero_si256();
        for (size_t j = 0; j < Count; j += 8) {
            __m256i data = _mm256_load_si256((__m256i*)&inKeys[j]);
            cnt0 = _mm256_sub_epi32(cnt0, _mm256_cmpgt_epi32(regg0, data));
            cnt1 = _mm256_sub_epi32(cnt1, _mm256_cmpgt_epi32(regg1, data));
            cnt2 = _mm256_sub_epi32(cnt2, _mm256_cmpgt_epi32(regg2, data));
            cnt3 = _mm256_sub_epi32(cnt3, _mm256_cmpgt_epi32(regg3, data));
        }
        if (!AssumeDistinct) {
            for (size_t j = 0; j < i; j += 4) {
                __m128i data = _mm_load_si128((__m128i*)&inKeys[j]);
                cnt0 = _mm256_sub_epi32(cnt0, _mm256_castsi128_si256(_mm_cmpeq_epi32(data, reg0)));
                cnt1 = _mm256_sub_epi32(cnt1, _mm256_castsi128_si256(_mm_cmpeq_epi32(data, reg1)));
                cnt2 = _mm256_sub_epi32(cnt2, _mm256_castsi128_si256(_mm_cmpeq_epi32(data, reg2)));
                cnt3 = _mm256_sub_epi32(cnt3, _mm256_castsi128_si256(_mm_cmpeq_epi32(data, reg3)));
            }
            //cnt0 = _mm256_sub_epi32(cnt0, _mm256_castsi128_si256((_mm_and_si128(_mm_cmplt_epi32(reg, reg0), _mm_setr_epi32( 0,  0,  0,  0))));
            cnt1 = _mm256_sub_epi32(cnt1, _mm256_castsi128_si256(_mm_and_si128(_mm_cmpeq_epi32(reg, reg1), _mm_setr_epi32(-1,  0,  0,  0))));
            cnt2 = _mm256_sub_epi32(cnt2, _mm256_castsi128_si256(_mm_and_si128(_mm_cmpeq_epi32(reg, reg2), _mm_setr_epi32(-1, -1,  0,  0))));
            cnt3 = _mm256_sub_epi32(cnt3, _mm256_castsi128_si256(_mm_and_si128(_mm_cmpeq_epi32(reg, reg3), _mm_setr_epi32(-1, -1, -1,  0))));
        }
        
        __m128i cnt0h = _mm_add_epi32(_mm256_castsi256_si128(cnt0), _mm256_extracti128_si256(cnt0, 1));
        __m128i cnt1h = _mm_add_epi32(_mm256_castsi256_si128(cnt1), _mm256_extracti128_si256(cnt1, 1));
        __m128i cnt2h = _mm_add_epi32(_mm256_castsi256_si128(cnt2), _mm256_extracti128_si256(cnt2, 1));
        __m128i cnt3h = _mm_add_epi32(_mm256_castsi256_si128(cnt3), _mm256_extracti128_si256(cnt3, 1));

        __m128i c01L = _mm_unpacklo_epi32(cnt0h, cnt1h);
        __m128i c01H = _mm_unpackhi_epi32(cnt0h, cnt1h);
        __m128i c23L = _mm_unpacklo_epi32(cnt2h, cnt3h);
        __m128i c23H = _mm_unpackhi_epi32(cnt2h, cnt3h);
        __m128i cntX = _mm_unpacklo_epi64(c01L, c23L);
        __m128i cntY = _mm_unpackhi_epi64(c01L, c23L);
        __m128i cntZ = _mm_unpacklo_epi64(c01H, c23H);
        __m128i cntW = _mm_unpackhi_epi64(c01H, c23H);
        __m128i cnt = _mm_add_epi32(_mm_add_epi32(cntX, cntY), _mm_add_epi32(cntZ, cntW));

        unsigned k0 = _mm_cvtsi128_si32(cnt   );
        unsigned k1 = _mm_extract_epi32(cnt, 1);
        unsigned k2 = _mm_extract_epi32(cnt, 2);
        unsigned k3 = _mm_extract_epi32(cnt, 3);
        dstKeys[k0] = inKeys[i+0];
        dstKeys[k1] = inKeys[i+1];
        dstKeys[k2] = inKeys[i+2];
        dstKeys[k3] = inKeys[i+3];
        if (WithValues) {
            dstVals[k0] = inVals[i+0];
            dstVals[k1] = inVals[i+1];
            dstVals[k2] = inVals[i+2];
            dstVals[k3] = inVals[i+3];
        }
    }
}

