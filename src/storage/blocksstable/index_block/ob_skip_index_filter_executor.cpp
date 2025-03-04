/**
 * Copyright (c) 2022 OceanBase
 * OceanBase is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX STORAGE
#include "storage/blocksstable/index_block/ob_skip_index_filter_executor.h"
namespace oceanbase
{
namespace blocksstable
{

int ObSkipIndexFilterExecutor::read_aggregate_data(const uint32_t col_idx,
                  common::ObIAllocator &allocator,
                  const share::schema::ObColumnParam *col_param,
                  const ObObjMeta &obj_meta,
                  ObStorageDatum &null_count,
                  ObStorageDatum &min_datum,
                  ObStorageDatum &max_datum)
{
  int ret = OB_SUCCESS;
  meta_.col_idx_ = col_idx;
  meta_.col_type_ = SK_IDX_NULL_COUNT;
  if (OB_FAIL(agg_row_reader_.read(meta_, null_count))) {
    LOG_WARN("Failed read agg null count", K(ret), K(meta_));
  } else if (FALSE_IT(meta_.col_type_ = SK_IDX_MIN)) {
  } else if (OB_FAIL(agg_row_reader_.read(meta_, min_datum))) {
    LOG_WARN("Failed read agg min datum", K(ret), K(meta_));
  } else if (FALSE_IT(meta_.col_type_ = SK_IDX_MAX)) {
  } else if (OB_FAIL(agg_row_reader_.read(meta_, max_datum))) {
    LOG_WARN("Failed read agg max datum", K(ret), K(meta_));
  } else if (!min_datum.is_null() &&
             OB_FAIL(pad_column(obj_meta, col_param, allocator, min_datum))) {
    LOG_WARN("Failed to pad column on min datum", K(ret));
  } else if (!max_datum.is_null() &&
             OB_FAIL(pad_column(obj_meta, col_param, allocator, max_datum))){
    LOG_WARN("Failed to pad column on max datum", K(ret));
  }
  LOG_DEBUG("[SKIP INDEX] read aggregate row", K(ret), K(null_count), K(min_datum), K(max_datum));
  return ret;
}

int ObSkipIndexFilterExecutor::falsifiable_pushdown_filter(
    const uint32_t col_idx,
    const ObObjMeta &obj_meta,
    const ObSkipIndexType index_type,
    const ObMicroIndexInfo &index_info,
    sql::ObWhiteFilterExecutor &filter,
    common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  reset();
  if (OB_UNLIKELY(!index_info.has_agg_data())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument", K(ret), K(index_info));
  } else if (OB_FAIL(agg_row_reader_.init(index_info.agg_row_buf_, index_info.agg_buf_size_))) {
    LOG_WARN("failed to init agg row reader", K(ret));
  } else {
    switch (index_type) {
      case ObSkipIndexType::MIN_MAX: {
        if (filter.is_filter_dynamic_node()) {
          sql::ObDynamicFilterExecutor &dynamic_filter =
              static_cast<sql::ObDynamicFilterExecutor &>(filter);
          if (!dynamic_filter.is_data_prepared()) {
            filter.get_filter_bool_mask().set_uncertain();
          } else if (dynamic_filter.is_filter_all_data()) {
            filter.get_filter_bool_mask().set_always_false();
          } else if (dynamic_filter.is_pass_all_data()) {
            filter.get_filter_bool_mask().set_always_true();
          } else if (OB_FAIL(filter_on_min_max(col_idx, index_info.get_row_count(),
              obj_meta, filter, allocator))) {
            LOG_WARN("Fail to filter on min_max for dynamic filter", K(ret), K(col_idx));
          }
        } else if (OB_FAIL(filter_on_min_max(col_idx, index_info.get_row_count(),
            obj_meta, filter, allocator))) {
          LOG_WARN("Fail to filter on min_max", K(ret), K(col_idx));
        }
        break;
      }
      default :
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("unsupported skip index type", K(ret), K(index_type));
        break;
    }
  }
  return ret;
}

int ObSkipIndexFilterExecutor::filter_on_min_max(
    const uint32_t col_idx,
    const uint64_t row_count,
    const ObObjMeta &obj_meta,
    sql::ObWhiteFilterExecutor &filter,
    common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  sql::ObBoolMask &fal_desc = filter.get_filter_bool_mask();
  const share::schema::ObColumnParam *col_param = filter.get_col_params().at(0);
  ObStorageDatum null_count;
  ObStorageDatum min_datum;
  ObStorageDatum max_datum;
  if (filter.is_cmp_op_with_null_ref_value()) {
    fal_desc.set_always_false();
  } else if (OB_FAIL(read_aggregate_data(col_idx, allocator, col_param,
                                  obj_meta, null_count, min_datum, max_datum))) {
    LOG_WARN("Failed to read min and max", K(ret), K(col_idx));
  } else if (null_count.is_null() && min_datum.is_null() && max_datum.is_null()) {
    // min max null_count all null, expect uncertain cause by progressive merge
    fal_desc.set_uncertain();
  } else if (null_count.is_null() || null_count.get_int() < 0 || null_count.get_int() > row_count ||
             min_datum.is_null() != max_datum.is_null()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("not correct min_max agg info", K(ret), K(col_idx), K(row_count),
             K(null_count), K(min_datum), K(max_datum));
  } else {
    // following three flags are mutually exclusive, only one can be true
    const bool is_all_null = null_count.get_int() == row_count;
    const bool is_all_not_null = null_count.get_int() == 0;
    const bool has_null = null_count.get_int() > 0 && null_count.get_int() < row_count;
    const bool is_min_max_null = min_datum.is_null() && max_datum.is_null(); //for unsupported data, eg: lob out row, json ...
    const sql::ObWhiteFilterOperatorType op_type = filter.get_op_type();
    switch (op_type) {
      case sql::WHITE_OP_NU: {
        if (is_all_not_null) {
          fal_desc.set_always_false();
        } else if (is_all_null) {
          fal_desc.set_always_true();
        } else {
          fal_desc.set_uncertain();
        }
        break;
      }
      case sql::WHITE_OP_NN: {
        if (is_all_null) {
          fal_desc.set_always_false();
        } else if (is_all_not_null) {
          fal_desc.set_always_true();
        } else {
          fal_desc.set_uncertain();
        }
        break;
      }
      case sql::WHITE_OP_EQ: {
        if (is_all_null) {
          fal_desc.set_always_false();
        } else if (is_min_max_null) {
          fal_desc.is_uncertain();
        } else if (OB_FAIL(eq_operator(filter, min_datum, max_datum, fal_desc))) {
          LOG_WARN("Failed to run EQ operator", K(ret));
        }
        break;
      }
      case sql::WHITE_OP_NE: {
        if (is_all_null) {
          fal_desc.set_always_false();
        } else if (is_min_max_null) {
          fal_desc.is_uncertain();
        } else if (OB_FAIL(ne_operator(filter, min_datum, max_datum, fal_desc))) {
          LOG_WARN("Failed to run NE operator", K(ret));
        }
        break;
      }
      case sql::WHITE_OP_GT: {
        if (is_all_null) {
          fal_desc.set_always_false();
        } else if (is_min_max_null) {
          fal_desc.is_uncertain();
        } else if (OB_FAIL(gt_operator(filter, min_datum, max_datum, fal_desc))) {
          LOG_WARN("Failed to run GT operator", K(ret));
        }
        break;
      }
      case sql::WHITE_OP_GE: {
        if (is_all_null) {
          fal_desc.set_always_false();
        } else if (is_min_max_null) {
          fal_desc.is_uncertain();
        } else if (OB_FAIL(ge_operator(filter, min_datum, max_datum, fal_desc))) {
          LOG_WARN("Failed to run GE operator", K(ret));
        }
        break;
      }
      case sql::WHITE_OP_LT: {
        if (is_all_null) {
          fal_desc.set_always_false();
        } else if (is_min_max_null) {
          fal_desc.is_uncertain();
        } else if (OB_FAIL(lt_operator(filter, min_datum, max_datum, fal_desc))) {
          LOG_WARN("Failed to run LT operator", K(ret));
        }
        break;
      }
      case sql::WHITE_OP_LE: {
        if (is_all_null) {
          fal_desc.set_always_false();
        } else if (is_min_max_null) {
          fal_desc.is_uncertain();
        } else if (OB_FAIL(le_operator(filter, min_datum, max_datum, fal_desc))) {
          LOG_WARN("Failed to run LE operator", K(ret));
        }
        break;
      }
      case sql::WHITE_OP_IN: {
        if (is_all_null) {
          fal_desc.set_always_false();
        } else if (is_min_max_null) {
          fal_desc.is_uncertain();
        } else if (OB_FAIL(in_operator(filter, min_datum, max_datum, fal_desc))) {
          LOG_WARN("Failed to run IN operator", K(ret));
        }
        break;
      }
      case sql::WHITE_OP_BT: {
        if (is_all_null) {
          fal_desc.set_always_false();
        } else if (is_min_max_null) {
          fal_desc.is_uncertain();
        } else if (OB_FAIL(bt_operator(filter, min_datum, max_datum, fal_desc))) {
          LOG_WARN("Failed to run BT operator", K(ret));
        }
        break;
      }
      default: {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("Unexpected filter pushdown operation type", K(ret), K(op_type));
      }
    } // end of switch
    if(OB_SUCC(ret)) {
      if (has_null && fal_desc.is_always_true()) {
        fal_desc.set_uncertain();
      }
    }
  }
  return ret;
}

inline int ObSkipIndexFilterExecutor::pad_column(const ObObjMeta &obj_meta,
                                          const share::schema::ObColumnParam *col_param,
                                          common::ObIAllocator &padding_alloc,
                                          blocksstable::ObStorageDatum &datum)
{
  int ret = OB_SUCCESS;
  if (obj_meta.is_fixed_len_char_type() && nullptr != col_param) {
    if (OB_FAIL(storage::pad_column(obj_meta, col_param->get_accuracy(),
                                    padding_alloc, datum))) {
      LOG_WARN("Failed to pad column", K(ret));
    }
  }
  return ret;
}

int ObSkipIndexFilterExecutor::eq_operator(const sql::ObWhiteFilterExecutor &filter,
                                           const common::ObDatum &min_datum,
                                           const common::ObDatum &max_datum,
                                           sql::ObBoolMask &fal_desc)
{
  int ret = OB_SUCCESS;
  const common::ObIArray<common::ObDatum> &datums = filter.get_datums();
  if (OB_UNLIKELY(datums.count() != 1 || filter.null_param_contained())){
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument for falsifiable EQ operator", K(ret), K(filter));
  } else {
    const ObDatum &ref_datum = datums.at(0);
    ObDatumCmpFuncType cmp_func = filter.cmp_func_;
    int min_cmp_res = 0;
    int max_cmp_res = 0;
    if (OB_FAIL(cmp_func(min_datum, ref_datum, min_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(min_datum), K(ref_datum));
    } else if (OB_FAIL(cmp_func(max_datum, ref_datum, max_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(max_datum), K(ref_datum));
    } else if (min_cmp_res > 0 || max_cmp_res < 0) {
      fal_desc.set_always_false();
    } else if (min_cmp_res == 0 && max_cmp_res == 0) {
      fal_desc.set_always_true();
    } else {
      fal_desc.set_uncertain();
    }
  }
  return ret;
}

int ObSkipIndexFilterExecutor::ne_operator(const sql::ObWhiteFilterExecutor &filter,
                                           const common::ObDatum &min_datum,
                                           const common::ObDatum &max_datum,
                                           sql::ObBoolMask &fal_desc)
{
  int ret = OB_SUCCESS;
  const common::ObIArray<common::ObDatum> &datums = filter.get_datums();
  if (OB_UNLIKELY(datums.count() != 1 || filter.null_param_contained())){
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument for falsifiable NE operator", K(ret), K(filter));
  } else {
    const ObDatum &ref_datum = datums.at(0);
    ObDatumCmpFuncType cmp_func = filter.cmp_func_;
    int min_cmp_res = 0;
    int max_cmp_res = 0;
    if (OB_FAIL(cmp_func(min_datum, ref_datum, min_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(min_datum), K(ref_datum));
    } else if (OB_FAIL(cmp_func(max_datum, ref_datum, max_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(max_datum), K(ref_datum));
    } else if (min_cmp_res == 0 && max_cmp_res == 0) {
      fal_desc.set_always_false();
    } else if (min_cmp_res > 0 || max_cmp_res < 0) {
      fal_desc.set_always_true();
    } else {
      fal_desc.set_uncertain();
    }
  }
  return ret;
}

int ObSkipIndexFilterExecutor::gt_operator(const sql::ObWhiteFilterExecutor &filter,
                                           const common::ObDatum &min_datum,
                                           const common::ObDatum &max_datum,
                                           sql::ObBoolMask &fal_desc)
{
  int ret = OB_SUCCESS;
  const common::ObIArray<common::ObDatum> &datums = filter.get_datums();
  if (OB_UNLIKELY(datums.count() != 1 || filter.null_param_contained())){
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument for falsifiable GT operator", K(ret), K(filter));
  } else {
    const ObDatum &ref_datum = datums.at(0);
    ObDatumCmpFuncType cmp_func = filter.cmp_func_;
    int min_cmp_res = 0;
    int max_cmp_res = 0;
    if (OB_FAIL(cmp_func(min_datum, ref_datum, min_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(min_datum), K(ref_datum));
    } else if (OB_FAIL(cmp_func(max_datum, ref_datum, max_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(max_datum), K(ref_datum));
    } else if (max_cmp_res < 0 || max_cmp_res == 0) {
      fal_desc.set_always_false();
    } else if (min_cmp_res > 0) {
      fal_desc.set_always_true();
    } else {
      fal_desc.set_uncertain();
    }
  }
  return ret;
}

int ObSkipIndexFilterExecutor::ge_operator(const sql::ObWhiteFilterExecutor &filter,
                                           const common::ObDatum &min_datum,
                                           const common::ObDatum &max_datum,
                                           sql::ObBoolMask &fal_desc)
{
  int ret = OB_SUCCESS;
  const common::ObIArray<common::ObDatum> &datums = filter.get_datums();
  if (OB_UNLIKELY(datums.count() != 1 || filter.null_param_contained())){
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument for falsifiable GE operator", K(ret), K(filter));
  } else {
    const ObDatum &ref_datum = datums.at(0);
    ObDatumCmpFuncType cmp_func = filter.cmp_func_;
    int min_cmp_res = 0;
    int max_cmp_res = 0;
    if (OB_FAIL(cmp_func(min_datum, ref_datum, min_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(min_datum), K(ref_datum));
    } else if (OB_FAIL(cmp_func(max_datum, ref_datum, max_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(max_datum), K(ref_datum));
    } else if (max_cmp_res < 0) {
      fal_desc.set_always_false();
    } else if (min_cmp_res > 0 || min_cmp_res == 0) {
      fal_desc.set_always_true();
    } else {
      fal_desc.set_uncertain();
    }
  }
  return ret;
}

int ObSkipIndexFilterExecutor::lt_operator(const sql::ObWhiteFilterExecutor &filter,
                                           const common::ObDatum &min_datum,
                                           const common::ObDatum &max_datum,
                                           sql::ObBoolMask &fal_desc)
{
  int ret = OB_SUCCESS;
  const common::ObIArray<common::ObDatum> &datums = filter.get_datums();
  if (OB_UNLIKELY(datums.count() != 1 || filter.null_param_contained())){
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument for falsifiable LT operator", K(ret), K(filter));
  } else {
    const ObDatum &ref_datum = datums.at(0);
    ObDatumCmpFuncType cmp_func = filter.cmp_func_;
    int min_cmp_res = 0;
    int max_cmp_res = 0;
    if (OB_FAIL(cmp_func(min_datum, ref_datum, min_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(min_datum), K(ref_datum));
    } else if (OB_FAIL(cmp_func(max_datum, ref_datum, max_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(max_datum), K(ref_datum));
    } else if (min_cmp_res > 0 || min_cmp_res == 0) {
      fal_desc.set_always_false();
    } else if (max_cmp_res < 0) {
      fal_desc.set_always_true();
    } else {
      fal_desc.set_uncertain();
    }
  }
  return ret;
}

int ObSkipIndexFilterExecutor::le_operator(const sql::ObWhiteFilterExecutor &filter,
                                           const common::ObDatum &min_datum,
                                           const common::ObDatum &max_datum,
                                           sql::ObBoolMask &fal_desc)
{
  int ret = OB_SUCCESS;
  const common::ObIArray<common::ObDatum> &datums = filter.get_datums();
  if (OB_UNLIKELY(datums.count() != 1 || filter.null_param_contained())){
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument for falsifiable LE operator", K(ret), K(filter));
  } else {
    const ObDatum &ref_datum = datums.at(0);
    bool max_prefix = max_datum.len_ == ObSkipIndexColMeta::MAX_SKIP_INDEX_COL_LENGTH;
    ObDatumCmpFuncType cmp_func = filter.cmp_func_;
    int min_cmp_res = 0;
    int max_cmp_res = 0;
    if (OB_FAIL(cmp_func(min_datum, ref_datum, min_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(min_datum), K(ref_datum));
    } else if (OB_FAIL(cmp_func(max_datum, ref_datum, max_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(max_datum), K(ref_datum));
    } else if (min_cmp_res > 0) {
      fal_desc.set_always_false();
    } else if (max_cmp_res < 0 || max_cmp_res == 0) {
      fal_desc.set_always_true();
    } else {
      fal_desc.set_uncertain();
    }
  }
  return ret;
}

int ObSkipIndexFilterExecutor::in_operator(const sql::ObWhiteFilterExecutor &filter,
                                           const common::ObDatum &min_datum,
                                           const common::ObDatum &max_datum,
                                           sql::ObBoolMask &fal_desc)
{
  int ret = OB_SUCCESS;
  const common::ObIArray<common::ObDatum> &datums = filter.get_datums();
  if (OB_UNLIKELY(datums.count() == 0 || filter.null_param_contained())){
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument for falsifiable IN operator", K(ret), K(filter));
  } else {
    const int ref_count = datums.count();
    ObDatumCmpFuncType cmp_func = filter.cmp_func_;
    int min_cmp_res;
    int max_cmp_res;
    bool falsifable_true = true;
    bool falsifable_false = false;
    for (int i = 0; OB_SUCC(ret) && i < ref_count; ++i) {
      if (OB_FAIL(cmp_func(min_datum, datums.at(i), min_cmp_res))) {
        LOG_WARN("Failed to compare datum", K(ret), K(min_datum), K(i), K(datums.at(i)));
      } else if (OB_FAIL(cmp_func(max_datum, datums.at(i), max_cmp_res))) {
        LOG_WARN("Failed to compare datum", K(ret), K(max_datum), K(i), K(datums.at(i)));
      } else {
          if (falsifable_true && ((min_cmp_res < 0 && max_cmp_res > 0) ||
              min_cmp_res == 0 || max_cmp_res == 0)) {
            falsifable_true = false;
          }
          if (!falsifable_false && min_cmp_res == 0 && max_cmp_res == 0) {
            falsifable_false = true;
          }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (falsifable_true) {
      fal_desc.set_always_false();
    } else if (falsifable_false) {
      fal_desc.set_always_true();
    } else {
      fal_desc.set_uncertain();
    }
  }
  return ret;
}

int ObSkipIndexFilterExecutor::bt_operator(const sql::ObWhiteFilterExecutor &filter,
                                           const common::ObDatum &min_datum,
                                           const common::ObDatum &max_datum,
                                           sql::ObBoolMask &fal_desc)
{
  int ret = OB_SUCCESS;
  const common::ObIArray<common::ObDatum> &datums = filter.get_datums();
  if (OB_UNLIKELY(datums.count() != 2 || filter.null_param_contained())){
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument for falsifiable bt operator", K(ret), K(filter));
  } else {
    const ObDatum &ref_left_datum = datums.at(0);
    const ObDatum &ref_right_datum = datums.at(1);

    ObDatumCmpFuncType cmp_func = filter.cmp_func_;
    int min_left_cmp_res = 0;
    int min_right_cmp_res = 0;
    int max_left_cmp_res = 0;
    int max_right_cmp_res = 0;

    if (OB_FAIL(cmp_func(min_datum, ref_left_datum, min_left_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(min_datum), K(ref_left_datum));
    } else if (OB_FAIL(cmp_func(min_datum, ref_right_datum, min_right_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(max_datum), K(ref_right_datum));
    } else if (OB_FAIL(cmp_func(max_datum, ref_left_datum, max_left_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(max_datum), K(ref_left_datum));
    } else if (OB_FAIL(cmp_func(max_datum, ref_right_datum, max_right_cmp_res))) {
      LOG_WARN("Failed to compare datum", K(ret), K(max_datum), K(ref_right_datum));
    } else if (min_right_cmp_res > 0 || max_left_cmp_res < 0) {
      fal_desc.set_always_false();
    } else if ((min_left_cmp_res > 0 || min_left_cmp_res == 0) &&
        (max_right_cmp_res < 0 || max_right_cmp_res == 0)) {
      fal_desc.set_always_true();
    } else {
      fal_desc.set_uncertain();
    }
  }
  return ret;
}
} // end namespace blocksstable
} // end namespace oceanbase
