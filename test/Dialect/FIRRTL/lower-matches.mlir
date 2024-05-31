// RUN: circt-opt --pass-pipeline="builtin.module(firrtl.circuit(firrtl.module(firrtl-lower-matches)))" %s | FileCheck %s

firrtl.circuit "LowerMatches" {

// CHECK-LABEL: firrtl.module @EmptyEnum
firrtl.module @EmptyEnum(in %enum : !firrtl.enum<>) {
  firrtl.match %enum : !firrtl.enum<> {
  }
// CHECK-NEXT: }
}

// CHECK-LABEL: firrtl.module @OneVariant
firrtl.module @OneVariant(in %enum : !firrtl.enum<a: uint<8>>, out %out : !firrtl.uint<8>) {
  // CHECK: %0 = firrtl.subtag %enum[a] : !firrtl.enum<a: uint<8>>
  // CHECK: %1 = firrtl.wrapSink %out
  // CHECK: firrtl.strictconnect %1, %0
  firrtl.match %enum : !firrtl.enum<a: uint<8>> {
    case a(%arg0) {
      %out_write = firrtl.wrapSink %out : !firrtl.uint<8>
      firrtl.strictconnect %out_write, %arg0 : !firrtl.uint<8>
    }
  }
}

// CHECK-LABEL: firrtl.module @LowerMatches
firrtl.module @LowerMatches(in %enum : !firrtl.enum<a: uint<8>, b: uint<8>, c: uint<8>>, out %out : !firrtl.uint<8>) {
   // CHECK-NEXT: %0 = firrtl.wrapSink %out
   // CHECK-NEXT: %1 = firrtl.istag %enum a : !firrtl.enum<a: uint<8>, b: uint<8>, c: uint<8>>
   // CHECK-NEXT: firrtl.when %1 : !firrtl.uint<1> {
   // CHECK-NEXT:   %2 = firrtl.subtag %enum[a] : !firrtl.enum<a: uint<8>, b: uint<8>, c: uint<8>>
   // CHECK-NEXT:   firrtl.strictconnect %0, %2
   // CHECK-NEXT: } else {
   // CHECK-NEXT:   %2 = firrtl.istag %enum b : !firrtl.enum<a: uint<8>, b: uint<8>, c: uint<8>>
   // CHECK-NEXT:   firrtl.when %2 : !firrtl.uint<1> {
   // CHECK-NEXT:     %3 = firrtl.subtag %enum[b] : !firrtl.enum<a: uint<8>, b: uint<8>, c: uint<8>>
   // CHECK-NEXT:     firrtl.strictconnect %0, %3
   // CHECK-NEXT:   } else {
   // CHECK-NEXT:     %3 = firrtl.subtag %enum[c] : !firrtl.enum<a: uint<8>, b: uint<8>, c: uint<8>>
   // CHECK-NEXT:     firrtl.strictconnect %0, %3
   // CHECK-NEXT:   }
   // CHECK-NEXT: }
  %out_write = firrtl.wrapSink %out : !firrtl.uint<8>
  firrtl.match %enum : !firrtl.enum<a: uint<8>, b: uint<8>, c: uint<8>> {
    case a(%arg0) {
      firrtl.strictconnect %out_write, %arg0 : !firrtl.uint<8>
    }
    case b(%arg0) {
      firrtl.strictconnect %out_write, %arg0 : !firrtl.uint<8>
    }
    case c(%arg0) {
      firrtl.strictconnect %out_write, %arg0 : !firrtl.uint<8>
    }
  }

}

// CHECK-LABEL: firrtl.module @ConstLowerMatches
firrtl.module @ConstLowerMatches(in %enum : !firrtl.const.enum<a: uint<8>, b: uint<8>, c: uint<8>>, out %out : !firrtl.const.uint<8>) {

  // CHECK-NEXT: %0 = firrtl.wrapSink %out
  // CHECK-NEXT: %1 = firrtl.istag %enum a : !firrtl.const.enum<a: uint<8>, b: uint<8>, c: uint<8>>
  // CHECK-NEXT: firrtl.when %1 : !firrtl.const.uint<1> {
  // CHECK-NEXT:   %2 = firrtl.subtag %enum[a] : !firrtl.const.enum<a: uint<8>, b: uint<8>, c: uint<8>>
  // CHECK-NEXT:   firrtl.strictconnect %0, %2 : !firrtl.const.uint<8>
  // CHECK-NEXT: } else {
  // CHECK-NEXT:   %2 = firrtl.istag %enum b : !firrtl.const.enum<a: uint<8>, b: uint<8>, c: uint<8>>
  // CHECK-NEXT:   firrtl.when %2 : !firrtl.const.uint<1> {
  // CHECK-NEXT:     %3 = firrtl.subtag %enum[b] : !firrtl.const.enum<a: uint<8>, b: uint<8>, c: uint<8>>
  // CHECK-NEXT:     firrtl.strictconnect %0, %3 : !firrtl.const.uint<8>
  // CHECK-NEXT:   } else {
  // CHECK-NEXT:     %3 = firrtl.subtag %enum[c] : !firrtl.const.enum<a: uint<8>, b: uint<8>, c: uint<8>>
  // CHECK-NEXT:     firrtl.strictconnect %0, %3 : !firrtl.const.uint<8>
  // CHECK-NEXT:   }
  // CHECK-NEXT: }
  %out_write = firrtl.wrapSink %out : !firrtl.const.uint<8>
  firrtl.match %enum : !firrtl.const.enum<a: uint<8>, b: uint<8>, c: uint<8>> {
    case a(%arg0) {
      firrtl.strictconnect %out_write, %arg0 : !firrtl.const.uint<8>
    }
    case b(%arg0) {
      firrtl.strictconnect %out_write, %arg0 : !firrtl.const.uint<8>
    }
    case c(%arg0) {
      firrtl.strictconnect %out_write, %arg0 : !firrtl.const.uint<8>
    }
  }

}
}
