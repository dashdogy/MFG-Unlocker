;
; Note: shader requires additional functionality:
;       Raytracing tier 1.1 features
;
;
; Input signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; no parameters
;
; Output signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; no parameters
; shader hash: 72e7251d1cd5285ca06d7d91bbf5ad7e
;
; Pipeline Runtime Information: 
;
;PSVRuntimeInfo:
; Compute Shader
; NumThreads=(64,1,1)
; MinimumExpectedWaveLaneCount: 0
; MaximumExpectedWaveLaneCount: 4294967295
; UsesViewID: false
; SigInputElements: 0
; SigOutputElements: 0
; SigPatchConstOrPrimElements: 0
; SigInputVectors: 0
; SigOutputVectors[0]: 0
; SigOutputVectors[1]: 0
; SigOutputVectors[2]: 0
; SigOutputVectors[3]: 0
; EntryFunctionName: CSMain
;
;
; Buffer Definitions:
;
; cbuffer Params
; {
;
;   struct Params
;   {
;
;       uint RayCount;                                ; Offset:    0
;       uint Unused0;                                 ; Offset:    4
;       uint Unused1;                                 ; Offset:    8
;       uint Unused2;                                 ; Offset:   12
;   
;   } Params;                                         ; Offset:    0 Size:    16
;
; }
;
; Resource bind info for Rays
; {
;
;   struct struct.RayIn
;   {
;
;       float3 origin;                                ; Offset:    0
;       float tmax;                                   ; Offset:   12
;       float3 direction;                             ; Offset:   16
;       uint mask;                                    ; Offset:   28
;   
;   } $Element;                                       ; Offset:    0 Size:    32
;
; }
;
; Resource bind info for g_NvidiaExt
; {
;
;   struct struct.NvShaderExtnStruct
;   {
;
;       uint opcode;                                  ; Offset:    0
;       uint rid;                                     ; Offset:    4
;       uint sid;                                     ; Offset:    8
;       uint4 dst1u;                                  ; Offset:   12
;       uint4 src3u;                                  ; Offset:   28
;       uint4 src4u;                                  ; Offset:   44
;       uint4 src5u;                                  ; Offset:   60
;       uint4 src0u;                                  ; Offset:   76
;       uint4 src1u;                                  ; Offset:   92
;       uint4 src2u;                                  ; Offset:  108
;       uint4 dst0u;                                  ; Offset:  124
;       uint markUavRef;                              ; Offset:  140
;       uint numOutputsForIncCounter;                 ; Offset:  144
;       float padding1[27];                           ; Offset:  148
;   
;   } $Element;                                       ; Offset:    0 Size:   256
;
; }
;
; Resource bind info for Results
; {
;
;   struct struct.LssOut
;   {
;
;       uint status;                                  ; Offset:    0
;       uint isLss;                                   ; Offset:    4
;       uint primitive;                               ; Offset:    8
;       uint instanceId;                              ; Offset:   12
;       float2 bary;                                  ; Offset:   16
;       float t;                                      ; Offset:   24
;       float hitParam;                               ; Offset:   28
;       float4 p;                                     ; Offset:   32
;       float4 q;                                     ; Offset:   48
;   
;   } $Element;                                       ; Offset:    0 Size:    64
;
; }
;
;
; Resource Bindings:
;
; Name                                 Type  Format         Dim      ID      HLSL Bind  Count
; ------------------------------ ---------- ------- ----------- ------- -------------- ------
; Params                            cbuffer      NA          NA     CB0            cb0     1
; Scene                             texture     i32         ras      T0             t0     1
; Rays                              texture  struct         r/o      T1             t1     1
; g_NvidiaExt                           UAV  struct     r/w+cnt      U0   u127,space99     1
; Results                               UAV  struct         r/w      U1             u0     1
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.ResBind = type { i32, i32, i32, i8 }
%dx.types.ResourceProperties = type { i32, i32 }
%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }
%dx.types.ResRet.f32 = type { float, float, float, float, i32 }
%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }
%struct.RaytracingAccelerationStructure = type { i32 }
%"class.StructuredBuffer<RayIn>" = type { %struct.RayIn }
%struct.RayIn = type { <3 x float>, float, <3 x float>, i32 }
%"class.RWStructuredBuffer<NvShaderExtnStruct>" = type { %struct.NvShaderExtnStruct }
%struct.NvShaderExtnStruct = type { i32, i32, i32, <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>, i32, i32, [27 x float] }
%"class.RWStructuredBuffer<LssOut>" = type { %struct.LssOut }
%struct.LssOut = type { i32, i32, i32, i32, <2 x float>, float, float, <4 x float>, <4 x float> }
%Params = type { i32, i32, i32, i32 }

define void @CSMain() {
  %1 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind { i32 0, i32 0, i32 0, i8 1 }, i32 0, i1 false)  ; CreateHandleFromBinding(bind,index,nonUniformIndex)
  %2 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind { i32 127, i32 127, i32 99, i8 1 }, i32 127, i1 false)  ; CreateHandleFromBinding(bind,index,nonUniformIndex)
  %3 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind { i32 1, i32 1, i32 0, i8 0 }, i32 1, i1 false)  ; CreateHandleFromBinding(bind,index,nonUniformIndex)
  %4 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind zeroinitializer, i32 0, i1 false)  ; CreateHandleFromBinding(bind,index,nonUniformIndex)
  %5 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind { i32 0, i32 0, i32 0, i8 2 }, i32 0, i1 false)  ; CreateHandleFromBinding(bind,index,nonUniformIndex)
  %6 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %5, %dx.types.ResourceProperties { i32 13, i32 16 })  ; AnnotateHandle(res,props)  resource: CBuffer
  %7 = call i32 @dx.op.threadId.i32(i32 93, i32 0)  ; ThreadId(component)
  %8 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %6, i32 0)  ; CBufferLoadLegacy(handle,regIndex)
  %9 = extractvalue %dx.types.CBufRet.i32 %8, 0
  %10 = icmp ult i32 %7, %9
  br i1 %10, label %11, label %103

; <label>:11                                      ; preds = %0
  %12 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %3, %dx.types.ResourceProperties { i32 524, i32 32 })  ; AnnotateHandle(res,props)  resource: StructuredBuffer<stride=32>
  %13 = call %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %12, i32 %7, i32 0, i8 7, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)
  %14 = extractvalue %dx.types.ResRet.f32 %13, 0
  %15 = extractvalue %dx.types.ResRet.f32 %13, 1
  %16 = extractvalue %dx.types.ResRet.f32 %13, 2
  %17 = call %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %12, i32 %7, i32 12, i8 1, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)
  %18 = extractvalue %dx.types.ResRet.f32 %17, 0
  %19 = call %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %12, i32 %7, i32 16, i8 7, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)
  %20 = extractvalue %dx.types.ResRet.f32 %19, 0
  %21 = extractvalue %dx.types.ResRet.f32 %19, 1
  %22 = extractvalue %dx.types.ResRet.f32 %19, 2
  %23 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %12, i32 %7, i32 28, i8 1, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)
  %24 = extractvalue %dx.types.ResRet.i32 %23, 0
  %25 = call i32 @dx.op.allocateRayQuery(i32 178, i32 1)  ; AllocateRayQuery(constRayFlags)
  %26 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %4, %dx.types.ResourceProperties { i32 16, i32 0 })  ; AnnotateHandle(res,props)  resource: RTAccelerationStructure
  call void @dx.op.rayQuery_TraceRayInline(i32 179, i32 %25, %dx.types.Handle %26, i32 0, i32 %24, float %14, float %15, float %16, float 0.000000e+00, float %20, float %21, float %22, float %18)  ; RayQuery_TraceRayInline(rayQueryHandle,accelerationStructure,rayFlags,instanceInclusionMask,origin_X,origin_Y,origin_Z,tMin,direction_X,direction_Y,direction_Z,tMax)
  br label %27

; <label>:27                                      ; preds = %27, %11
  %28 = call i1 @dx.op.rayQuery_Proceed.i1(i32 180, i32 %25)  ; RayQuery_Proceed(rayQueryHandle)
  br i1 %28, label %27, label %29

; <label>:29                                      ; preds = %27
  %30 = call i32 @dx.op.rayQuery_StateScalar.i32(i32 184, i32 %25)  ; RayQuery_CommittedStatus(rayQueryHandle)
  %31 = icmp eq i32 %30, 1
  br i1 %31, label %32, label %85

; <label>:32                                      ; preds = %29
  %33 = call i32 @dx.op.rayQuery_StateScalar.i32(i32 195, i32 %25)  ; RayQuery_RayFlags(rayQueryHandle)
  %34 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %35 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %34, i8 1)  ; BufferUpdateCounter(uav,inc)
  %36 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 4620, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256>
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %36, i32 %35, i32 0, i32 114, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  %37 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 4620, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256>
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %37, i32 %35, i32 76, i32 %33, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  %38 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %39 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %38, i8 1)  ; BufferUpdateCounter(uav,inc)
  %40 = icmp ne i32 %39, 0
  %41 = zext i1 %40 to i32
  %42 = call i32 @dx.op.rayQuery_StateScalar.i32(i32 210, i32 %25)  ; RayQuery_CommittedPrimitiveIndex(rayQueryHandle)
  %43 = call i32 @dx.op.rayQuery_StateScalar.i32(i32 208, i32 %25)  ; RayQuery_CommittedInstanceID(rayQueryHandle)
  %44 = call float @dx.op.rayQuery_StateVector.f32(i32 194, i32 %25, i8 0)  ; RayQuery_CommittedTriangleBarycentrics(rayQueryHandle,component)
  %45 = call float @dx.op.rayQuery_StateVector.f32(i32 194, i32 %25, i8 1)  ; RayQuery_CommittedTriangleBarycentrics(rayQueryHandle,component)
  %46 = call float @dx.op.rayQuery_StateScalar.f32(i32 200, i32 %25)  ; RayQuery_CommittedRayT(rayQueryHandle)
  br i1 %40, label %47, label %85

; <label>:47                                      ; preds = %32
  %48 = call i32 @dx.op.rayQuery_StateScalar.i32(i32 195, i32 %25)  ; RayQuery_RayFlags(rayQueryHandle)
  %49 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %50 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %49, i8 1)  ; BufferUpdateCounter(uav,inc)
  %51 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 4620, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256>
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %51, i32 %50, i32 0, i32 117, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  %52 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 4620, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256>
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %52, i32 %50, i32 76, i32 %48, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  %53 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %54 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %53, i8 1)  ; BufferUpdateCounter(uav,inc)
  %55 = bitcast i32 %54 to float
  %56 = call i32 @dx.op.rayQuery_StateScalar.i32(i32 195, i32 %25)  ; RayQuery_RayFlags(rayQueryHandle)
  %57 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %58 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %57, i8 1)  ; BufferUpdateCounter(uav,inc)
  %59 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 4620, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256>
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %59, i32 %58, i32 0, i32 106, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  %60 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 4620, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256>
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %60, i32 %58, i32 76, i32 %56, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  %61 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %62 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %61, i8 1)  ; BufferUpdateCounter(uav,inc)
  %63 = bitcast i32 %62 to float
  %64 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %65 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %64, i8 1)  ; BufferUpdateCounter(uav,inc)
  %66 = bitcast i32 %65 to float
  %67 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %68 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %67, i8 1)  ; BufferUpdateCounter(uav,inc)
  %69 = bitcast i32 %68 to float
  %70 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %71 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %70, i8 1)  ; BufferUpdateCounter(uav,inc)
  %72 = bitcast i32 %71 to float
  %73 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %74 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %73, i8 1)  ; BufferUpdateCounter(uav,inc)
  %75 = bitcast i32 %74 to float
  %76 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %77 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %76, i8 1)  ; BufferUpdateCounter(uav,inc)
  %78 = bitcast i32 %77 to float
  %79 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %80 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %79, i8 1)  ; BufferUpdateCounter(uav,inc)
  %81 = bitcast i32 %80 to float
  %82 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %83 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %82, i8 1)  ; BufferUpdateCounter(uav,inc)
  %84 = bitcast i32 %83 to float
  br label %85

; <label>:85                                      ; preds = %47, %32, %29
  %86 = phi i32 [ 1, %47 ], [ 1, %32 ], [ 0, %29 ]
  %87 = phi i32 [ %41, %47 ], [ %41, %32 ], [ 0, %29 ]
  %88 = phi i32 [ %42, %47 ], [ %42, %32 ], [ 0, %29 ]
  %89 = phi i32 [ %43, %47 ], [ %43, %32 ], [ 0, %29 ]
  %90 = phi float [ %44, %47 ], [ %44, %32 ], [ 0.000000e+00, %29 ]
  %91 = phi float [ %45, %47 ], [ %45, %32 ], [ 0.000000e+00, %29 ]
  %92 = phi float [ %46, %47 ], [ %46, %32 ], [ 0.000000e+00, %29 ]
  %93 = phi float [ %55, %47 ], [ 0.000000e+00, %32 ], [ 0.000000e+00, %29 ]
  %94 = phi float [ %63, %47 ], [ 0.000000e+00, %32 ], [ 0.000000e+00, %29 ]
  %95 = phi float [ %66, %47 ], [ 0.000000e+00, %32 ], [ 0.000000e+00, %29 ]
  %96 = phi float [ %69, %47 ], [ 0.000000e+00, %32 ], [ 0.000000e+00, %29 ]
  %97 = phi float [ %72, %47 ], [ 0.000000e+00, %32 ], [ 0.000000e+00, %29 ]
  %98 = phi float [ %75, %47 ], [ 0.000000e+00, %32 ], [ 0.000000e+00, %29 ]
  %99 = phi float [ %78, %47 ], [ 0.000000e+00, %32 ], [ 0.000000e+00, %29 ]
  %100 = phi float [ %81, %47 ], [ 0.000000e+00, %32 ], [ 0.000000e+00, %29 ]
  %101 = phi float [ %84, %47 ], [ 0.000000e+00, %32 ], [ 0.000000e+00, %29 ]
  %102 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %1, %dx.types.ResourceProperties { i32 4620, i32 64 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=64>
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %102, i32 %7, i32 0, i32 %86, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %102, i32 %7, i32 4, i32 %87, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %102, i32 %7, i32 8, i32 %88, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %102, i32 %7, i32 12, i32 %89, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %102, i32 %7, i32 16, float %90, float %91, float undef, float undef, i8 3, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %102, i32 %7, i32 24, float %92, float undef, float undef, float undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %102, i32 %7, i32 28, float %93, float undef, float undef, float undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %102, i32 %7, i32 32, float %94, float %95, float %96, float %97, i8 15, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %102, i32 %7, i32 48, float %98, float %99, float %100, float %101, i8 15, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  br label %103

; <label>:103                                     ; preds = %85, %0
  ret void
}

; Function Attrs: nounwind readnone
declare i32 @dx.op.threadId.i32(i32, i32) #0

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, i8, i32) #1

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32, %dx.types.Handle, i32, i32, i8, i32) #1

; Function Attrs: nounwind
declare void @dx.op.rawBufferStore.f32(i32, %dx.types.Handle, i32, i32, float, float, float, float, i8, i32) #2

; Function Attrs: nounwind
declare void @dx.op.rawBufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i8, i32) #2

; Function Attrs: nounwind
declare i32 @dx.op.bufferUpdateCounter(i32, %dx.types.Handle, i8) #2

; Function Attrs: nounwind readonly
declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32) #1

; Function Attrs: nounwind
declare i32 @dx.op.allocateRayQuery(i32, i32) #2

; Function Attrs: nounwind
declare void @dx.op.rayQuery_TraceRayInline(i32, i32, %dx.types.Handle, i32, i32, float, float, float, float, float, float, float, float) #2

; Function Attrs: nounwind readonly
declare float @dx.op.rayQuery_StateScalar.f32(i32, i32) #1

; Function Attrs: nounwind readonly
declare float @dx.op.rayQuery_StateVector.f32(i32, i32, i8) #1

; Function Attrs: nounwind readonly
declare i32 @dx.op.rayQuery_StateScalar.i32(i32, i32) #1

; Function Attrs: nounwind
declare i1 @dx.op.rayQuery_Proceed.i1(i32, i32) #2

; Function Attrs: nounwind readnone
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #0

; Function Attrs: nounwind readnone
declare %dx.types.Handle @dx.op.createHandleFromBinding(i32, %dx.types.ResBind, i32, i1) #0

attributes #0 = { nounwind readnone }
attributes #1 = { nounwind readonly }
attributes #2 = { nounwind }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!2}
!dx.shaderModel = !{!3}
!dx.resources = !{!4}
!dx.entryPoints = !{!17}

!0 = !{!"dxcoob 1.9.2602.17 (21d28f727)"}
!1 = !{i32 1, i32 6}
!2 = !{i32 1, i32 9}
!3 = !{!"cs", i32 6, i32 6}
!4 = !{!5, !10, !15, null}
!5 = !{!6, !8}
!6 = !{i32 0, %struct.RaytracingAccelerationStructure* undef, !"", i32 0, i32 0, i32 1, i32 16, i32 0, !7}
!7 = !{i32 0, i32 4}
!8 = !{i32 1, %"class.StructuredBuffer<RayIn>"* undef, !"", i32 0, i32 1, i32 1, i32 12, i32 0, !9}
!9 = !{i32 1, i32 32}
!10 = !{!11, !13}
!11 = !{i32 0, %"class.RWStructuredBuffer<NvShaderExtnStruct>"* undef, !"", i32 99, i32 127, i32 1, i32 12, i1 false, i1 true, i1 false, !12}
!12 = !{i32 1, i32 256}
!13 = !{i32 1, %"class.RWStructuredBuffer<LssOut>"* undef, !"", i32 0, i32 0, i32 1, i32 12, i1 false, i1 false, i1 false, !14}
!14 = !{i32 1, i32 64}
!15 = !{!16}
!16 = !{i32 0, %Params* undef, !"", i32 0, i32 0, i32 1, i32 16, null}
!17 = !{void ()* @CSMain, !"CSMain", null, !4, !18}
!18 = !{i32 0, i64 33554448, i32 4, !19}
!19 = !{i32 64, i32 1, i32 1}
