/* test_blazeface_decode — covers the BlazeFace anchor + bbox
 * regression + NMS decoder. */
#include <doctest/doctest.h>

#include "compose/blazeface_decode.hpp"
#include "inference/runtime.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr int A = me::compose::kBlazefaceAnchorCount;
constexpr int R = me::compose::kBlazefaceRegressorsPerAnchor;

me::inference::Tensor make_regressors_zero() {
    me::inference::Tensor t;
    t.shape = { 1, A, R };
    t.dtype = me::inference::Dtype::Float32;
    t.bytes.assign(static_cast<std::size_t>(A) * R * 4, 0);
    return t;
}

me::inference::Tensor make_classificators_zero() {
    me::inference::Tensor t;
    t.shape = { 1, A, 1 };
    t.dtype = me::inference::Dtype::Float32;
    t.bytes.assign(static_cast<std::size_t>(A) * 4, 0);
    return t;
}

void set_regressor(me::inference::Tensor& t, int anchor_idx,
                    float dx, float dy, float dw, float dh) {
    auto* fp = reinterpret_cast<float*>(t.bytes.data());
    const std::size_t base = static_cast<std::size_t>(anchor_idx) * R;
    fp[base + 0] = dx;
    fp[base + 1] = dy;
    fp[base + 2] = dw;
    fp[base + 3] = dh;
}

void set_classificator(me::inference::Tensor& t, int anchor_idx,
                        float logit) {
    auto* fp = reinterpret_cast<float*>(t.bytes.data());
    fp[anchor_idx] = logit;
}

}  // namespace

TEST_CASE("decode_blazeface_bboxes: null out rejected") {
    auto regs = make_regressors_zero();
    auto clas = make_classificators_zero();
    me::compose::BlazefaceDecodeParams dp;
    CHECK(me::compose::decode_blazeface_bboxes(
              regs, clas, 640, 480, dp, nullptr, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("decode_blazeface_bboxes: bad frame dims rejected") {
    auto regs = make_regressors_zero();
    auto clas = make_classificators_zero();
    me::compose::BlazefaceDecodeParams dp;
    std::vector<me::compose::Bbox> out;
    CHECK(me::compose::decode_blazeface_bboxes(
              regs, clas, 0, 480, dp, &out, nullptr) == ME_E_INVALID_ARG);
    CHECK(me::compose::decode_blazeface_bboxes(
              regs, clas, 640, -1, dp, &out, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("decode_blazeface_bboxes: bad thresholds rejected") {
    auto regs = make_regressors_zero();
    auto clas = make_classificators_zero();
    std::vector<me::compose::Bbox> out;

    me::compose::BlazefaceDecodeParams dp;
    dp.confidence_threshold = -0.1f;
    CHECK(me::compose::decode_blazeface_bboxes(
              regs, clas, 640, 480, dp, &out, nullptr) == ME_E_INVALID_ARG);

    dp.confidence_threshold = 1.5f;
    CHECK(me::compose::decode_blazeface_bboxes(
              regs, clas, 640, 480, dp, &out, nullptr) == ME_E_INVALID_ARG);

    dp.confidence_threshold = 0.5f;
    dp.iou_threshold = 0.0f;
    CHECK(me::compose::decode_blazeface_bboxes(
              regs, clas, 640, 480, dp, &out, nullptr) == ME_E_INVALID_ARG);

    dp.iou_threshold = 1.5f;
    CHECK(me::compose::decode_blazeface_bboxes(
              regs, clas, 640, 480, dp, &out, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("decode_blazeface_bboxes: wrong shapes rejected") {
    me::inference::Tensor bad_regs;
    bad_regs.shape = { 1, A, 4 };  /* should be 16 */
    bad_regs.dtype = me::inference::Dtype::Float32;
    bad_regs.bytes.assign(static_cast<std::size_t>(A) * 4 * 4, 0);
    auto clas = make_classificators_zero();
    me::compose::BlazefaceDecodeParams dp;
    std::vector<me::compose::Bbox> out;
    CHECK(me::compose::decode_blazeface_bboxes(
              bad_regs, clas, 640, 480, dp, &out, nullptr) == ME_E_INVALID_ARG);

    auto regs = make_regressors_zero();
    me::inference::Tensor bad_clas;
    bad_clas.shape = { 1, A, 2 };  /* should be 1 */
    bad_clas.dtype = me::inference::Dtype::Float32;
    bad_clas.bytes.assign(static_cast<std::size_t>(A) * 2 * 4, 0);
    CHECK(me::compose::decode_blazeface_bboxes(
              regs, bad_clas, 640, 480, dp, &out, nullptr) == ME_E_INVALID_ARG);
}

TEST_CASE("decode_blazeface_bboxes: all-zero confidences → no detections") {
    auto regs = make_regressors_zero();
    auto clas = make_classificators_zero();  /* sigmoid(0) = 0.5, default
                                                threshold 0.5 → drops */
    me::compose::BlazefaceDecodeParams dp;
    std::vector<me::compose::Bbox> out;
    REQUIRE(me::compose::decode_blazeface_bboxes(
                regs, clas, 640, 480, dp, &out, nullptr) == ME_OK);
    CHECK(out.empty());
}

TEST_CASE("decode_blazeface_bboxes: single positive anchor → one detection") {
    auto regs = make_regressors_zero();
    auto clas = make_classificators_zero();

    /* Pick anchor 0 (center near (0.03125, 0.03125) on layer 0,
     * 16x16 grid). Set high confidence + small bbox dims. */
    set_classificator(clas, 0, /*logit=*/10.0f);  /* sigmoid ≈ 1.0 */
    /* dx=dy=0 (no offset from anchor center), dw=dh=20 px (16% of 128 input). */
    set_regressor(regs, 0, 0.0f, 0.0f, 20.0f, 20.0f);

    me::compose::BlazefaceDecodeParams dp;
    std::vector<me::compose::Bbox> out;
    REQUIRE(me::compose::decode_blazeface_bboxes(
                regs, clas, 1280, 720, dp, &out, nullptr) == ME_OK);
    REQUIRE(out.size() == 1);
    /* Anchor 0 center: (0.5/16, 0.5/16) ≈ (0.03125, 0.03125)
     * → pixel center: (40, 22.5). bbox half-size: 20/128/2 ≈ 0.078
     * → frame half-size: ~100 px wide, ~56 px tall.
     * The bbox should land near the top-left corner. */
    CHECK(out[0].width()  > 0);
    CHECK(out[0].height() > 0);
    CHECK(out[0].x0       < 200);
    CHECK(out[0].y0       < 100);
}

TEST_CASE("decode_blazeface_bboxes: NMS suppresses overlapping detections") {
    auto regs = make_regressors_zero();
    auto clas = make_classificators_zero();

    /* Set 3 adjacent anchors on layer 0 (cells 0..2 in row 0)
     * to high confidence with overlapping bboxes — they all
     * regress to the same cell (cx=0.0625, dy=dw=dh same). */
    for (int i = 0; i < 3; ++i) {
        set_classificator(clas, i, 10.0f);  /* high confidence */
        set_regressor(regs, i, 0.0f, 0.0f, 60.0f, 60.0f);
    }

    me::compose::BlazefaceDecodeParams dp;
    dp.iou_threshold = 0.3f;
    std::vector<me::compose::Bbox> out;
    REQUIRE(me::compose::decode_blazeface_bboxes(
                regs, clas, 1280, 720, dp, &out, nullptr) == ME_OK);
    /* Three overlapping bboxes near the same anchor should NMS
     * down to 1 (or at most 2 if first-and-last barely overlap). */
    CHECK(out.size() <= 2);
    CHECK(out.size() >= 1);
}

TEST_CASE("decode_blazeface_bboxes: confidence threshold gates detections") {
    auto regs = make_regressors_zero();
    auto clas = make_classificators_zero();

    /* sigmoid(0.5) ≈ 0.622. With default threshold 0.5 it
     * passes; with threshold 0.7 it should drop. */
    set_classificator(clas, 0, 0.5f);
    set_regressor(regs, 0, 0.0f, 0.0f, 20.0f, 20.0f);

    me::compose::BlazefaceDecodeParams dp_lo;
    dp_lo.confidence_threshold = 0.5f;
    std::vector<me::compose::Bbox> out_lo;
    REQUIRE(me::compose::decode_blazeface_bboxes(
                regs, clas, 1280, 720, dp_lo, &out_lo, nullptr) == ME_OK);
    CHECK(!out_lo.empty());

    me::compose::BlazefaceDecodeParams dp_hi;
    dp_hi.confidence_threshold = 0.7f;
    std::vector<me::compose::Bbox> out_hi;
    REQUIRE(me::compose::decode_blazeface_bboxes(
                regs, clas, 1280, 720, dp_hi, &out_hi, nullptr) == ME_OK);
    CHECK(out_hi.empty());
}

TEST_CASE("decode_blazeface_bboxes: 2D classificators shape accepted") {
    auto regs = make_regressors_zero();
    me::inference::Tensor clas;
    clas.shape = { 1, A };
    clas.dtype = me::inference::Dtype::Float32;
    clas.bytes.assign(static_cast<std::size_t>(A) * 4, 0);

    me::compose::BlazefaceDecodeParams dp;
    std::vector<me::compose::Bbox> out;
    REQUIRE(me::compose::decode_blazeface_bboxes(
                regs, clas, 640, 480, dp, &out, nullptr) == ME_OK);
    CHECK(out.empty());  /* zero confidences → no detections */
}

TEST_CASE("decode_blazeface_bboxes: determinism") {
    auto regs = make_regressors_zero();
    auto clas = make_classificators_zero();
    /* Set a few high-confidence anchors so NMS has actual work. */
    set_classificator(clas, 0,   5.0f);
    set_classificator(clas, 100, 4.0f);
    set_classificator(clas, 500, 3.0f);
    set_regressor(regs, 0,   0.0f, 0.0f, 30.0f, 30.0f);
    set_regressor(regs, 100, 5.0f, 5.0f, 25.0f, 25.0f);
    set_regressor(regs, 500, 0.0f, 0.0f, 40.0f, 40.0f);

    me::compose::BlazefaceDecodeParams dp;
    std::vector<me::compose::Bbox> a, b;
    REQUIRE(me::compose::decode_blazeface_bboxes(
                regs, clas, 1280, 720, dp, &a, nullptr) == ME_OK);
    REQUIRE(me::compose::decode_blazeface_bboxes(
                regs, clas, 1280, 720, dp, &b, nullptr) == ME_OK);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].x0 == b[i].x0);
        CHECK(a[i].y0 == b[i].y0);
        CHECK(a[i].x1 == b[i].x1);
        CHECK(a[i].y1 == b[i].y1);
    }
}
