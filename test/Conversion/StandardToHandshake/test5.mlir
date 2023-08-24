// NOTE: Assertions have been autogenerated by utils/update_mlir_test_checks.py
// RUN: circt-opt -lower-std-to-handshake %s | FileCheck %s
// CHECK-LABEL:   handshake.func @dfs_block_order(
// CHECK-SAME:                                    %[[VAL_0:.*]]: none, ...) -> (i32, none) attributes {argNames = ["inCtrl"], resNames = ["out0", "outCtrl"]} {
// CHECK:           %[[VAL_1:.*]]:2 = fork [2] %[[VAL_0]] : none
// CHECK:           %[[VAL_2:.*]] = constant %[[VAL_1]]#0 {value = 42 : i32} : i32
// CHECK:           %[[VAL_3:.*]] = br %[[VAL_1]]#1 : none
// CHECK:           %[[VAL_4:.*]] = br %[[VAL_2]] : i32
// CHECK:           %[[VAL_5:.*]] = merge %[[VAL_6:.*]] : i32
// CHECK:           %[[VAL_7:.*]] = merge %[[VAL_8:.*]] : i32
// CHECK:           %[[VAL_9:.*]], %[[VAL_10:.*]] = control_merge %[[VAL_11:.*]] : none
// CHECK:           sink %[[VAL_10]] : index
// CHECK:           %[[VAL_12:.*]] = arith.addi %[[VAL_5]], %[[VAL_7]] : i32
// CHECK:           %[[VAL_13:.*]] = merge %[[VAL_4]] : i32
// CHECK:           %[[VAL_14:.*]], %[[VAL_15:.*]] = control_merge %[[VAL_3]] : none
// CHECK:           %[[VAL_16:.*]]:2 = fork [2] %[[VAL_14]] : none
// CHECK:           sink %[[VAL_15]] : index
// CHECK:           %[[VAL_17:.*]] = constant %[[VAL_16]]#0 {value = 55 : i32} : i32
// CHECK:           %[[VAL_6]] = br %[[VAL_13]] : i32
// CHECK:           %[[VAL_11]] = br %[[VAL_16]]#1 : none
// CHECK:           %[[VAL_8]] = br %[[VAL_17]] : i32
// CHECK:           return %[[VAL_12]], %[[VAL_9]] : i32, none
// CHECK:         }
func.func @dfs_block_order() -> (i32) {
  %0 = arith.constant 42 : i32
  cf.br ^bb2
^bb1:
  %2 = arith.addi %0, %1 : i32
  return %2 : i32
^bb2:
  %1 = arith.constant 55 : i32
  cf.br ^bb1
}