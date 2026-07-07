typedef int64_t anchor_idx_t;
typedef int32_t score_t;
typedef int32_t parent_t;

struct control_t {
    float avg_qspan;
    uint16_t tile_num;
    bool is_new_read;
    int n_r;
};
struct return_t {
    score_t  score;
    parent_t parent;
};

struct range_t{
    int32_t start;
    int32_t end;
};