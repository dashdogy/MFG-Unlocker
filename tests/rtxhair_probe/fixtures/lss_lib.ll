; shader hash: 171f7b8f5577149776d74d5c9000607a
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
%struct.Payload = type { i32 }
%dx.types.ResourceProperties = type { i32, i32 }
%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }
%dx.types.ResRet.f32 = type { float, float, float, float, i32 }
%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
%struct.RaytracingAccelerationStructure = type { i32 }
%"class.StructuredBuffer<RayIn>" = type { %struct.RayIn }
%struct.RayIn = type { <3 x float>, float, <3 x float>, i32 }
%"class.RWStructuredBuffer<NvShaderExtnStruct>" = type { %struct.NvShaderExtnStruct }
%struct.NvShaderExtnStruct = type { i32, i32, i32, <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>, i32, i32, [27 x float] }
%"class.RWStructuredBuffer<LssOut>" = type { %struct.LssOut }
%struct.LssOut = type { i32, i32, i32, i32, <2 x float>, float, float, <4 x float>, <4 x float> }
%Params = type { i32, i32, i32, i32 }

@"\01?g_NvidiaExt@@3V?$RWStructuredBuffer@UNvShaderExtnStruct@@@@A" = external constant %dx.types.Handle, align 4
@"\01?Scene@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?Rays@@3V?$StructuredBuffer@URayIn@@@@A" = external constant %dx.types.Handle, align 4
@"\01?Results@@3V?$RWStructuredBuffer@ULssOut@@@@A" = external constant %dx.types.Handle, align 4
@Params = external constant %dx.types.Handle

; Function Attrs: nounwind
define void @"\01?RayGen@@YAXXZ"() #0 {
  %1 = load %dx.types.Handle, %dx.types.Handle* @Params, align 4
  %2 = alloca %struct.Payload, align 8
  %3 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %1)  ; CreateHandleForLib(Resource)
  %4 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %3, %dx.types.ResourceProperties { i32 13, i32 16 })  ; AnnotateHandle(res,props)  resource: CBuffer
  %5 = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 0)  ; DispatchRaysIndex(col)
  %6 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %4, i32 0)  ; CBufferLoadLegacy(handle,regIndex)
  %7 = extractvalue %dx.types.CBufRet.i32 %6, 0
  %8 = icmp ult i32 %5, %7
  br i1 %8, label %9, label %29

; <label>:9                                       ; preds = %0
  %10 = load %dx.types.Handle, %dx.types.Handle* @"\01?Scene@@3URaytracingAccelerationStructure@@A", align 4
  %11 = load %dx.types.Handle, %dx.types.Handle* @"\01?Rays@@3V?$StructuredBuffer@URayIn@@@@A", align 4
  %12 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %11)  ; CreateHandleForLib(Resource)
  %13 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %12, %dx.types.ResourceProperties { i32 524, i32 32 })  ; AnnotateHandle(res,props)  resource: StructuredBuffer<stride=32>
  %14 = call %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %13, i32 %5, i32 0, i8 7, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)
  %15 = extractvalue %dx.types.ResRet.f32 %14, 0
  %16 = extractvalue %dx.types.ResRet.f32 %14, 1
  %17 = extractvalue %dx.types.ResRet.f32 %14, 2
  %18 = call %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %13, i32 %5, i32 12, i8 1, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)
  %19 = extractvalue %dx.types.ResRet.f32 %18, 0
  %20 = call %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %13, i32 %5, i32 16, i8 7, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)
  %21 = extractvalue %dx.types.ResRet.f32 %20, 0
  %22 = extractvalue %dx.types.ResRet.f32 %20, 1
  %23 = extractvalue %dx.types.ResRet.f32 %20, 2
  %24 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %13, i32 %5, i32 28, i8 1, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)
  %25 = extractvalue %dx.types.ResRet.i32 %24, 0
  %26 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %10)  ; CreateHandleForLib(Resource)
  %27 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %26, %dx.types.ResourceProperties { i32 16, i32 0 })  ; AnnotateHandle(res,props)  resource: RTAccelerationStructure
  %28 = getelementptr inbounds %struct.Payload, %struct.Payload* %2, i32 0, i32 0
  store i32 0, i32* %28, align 8
  call void @dx.op.traceRay.struct.Payload(i32 157, %dx.types.Handle %27, i32 1, i32 %25, i32 0, i32 1, i32 0, float %15, float %16, float %17, float 0.000000e+00, float %21, float %22, float %23, float %19, %struct.Payload* nonnull %2)  ; TraceRay(AccelerationStructure,RayFlags,InstanceInclusionMask,RayContributionToHitGroupIndex,MultiplierForGeometryContributionToShaderIndex,MissShaderIndex,Origin_X,Origin_Y,Origin_Z,TMin,Direction_X,Direction_Y,Direction_Z,TMax,payload)
  br label %29

; <label>:29                                      ; preds = %9, %0
  ret void
}

; Function Attrs: nounwind
define void @"\01?Miss@@YAXUPayload@@@Z"(%struct.Payload* noalias nocapture %payload) #0 {
  %1 = load %dx.types.Handle, %dx.types.Handle* @"\01?Results@@3V?$RWStructuredBuffer@ULssOut@@@@A", align 4
  %2 = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 0)  ; DispatchRaysIndex(col)
  %3 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %1)  ; CreateHandleForLib(Resource)
  %4 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %3, %dx.types.ResourceProperties { i32 4620, i32 64 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=64>
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %4, i32 %2, i32 0, i32 0, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %4, i32 %2, i32 4, i32 0, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %4, i32 %2, i32 8, i32 0, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %4, i32 %2, i32 12, i32 0, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %4, i32 %2, i32 16, float 0.000000e+00, float 0.000000e+00, float undef, float undef, i8 3, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %4, i32 %2, i32 24, float 0.000000e+00, float undef, float undef, float undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %4, i32 %2, i32 28, float 0.000000e+00, float undef, float undef, float undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %4, i32 %2, i32 32, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, i8 15, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %4, i32 %2, i32 48, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, i8 15, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  ret void
}

; Function Attrs: nounwind
define void @"\01?ClosestHit@@YAXUPayload@@UBuiltInTriangleIntersectionAttributes@@@Z"(%struct.Payload* noalias nocapture %payload, %struct.BuiltInTriangleIntersectionAttributes* nocapture readonly %attributes) #0 {
  %1 = load %dx.types.Handle, %dx.types.Handle* @"\01?Results@@3V?$RWStructuredBuffer@ULssOut@@@@A", align 4
  %2 = load %dx.types.Handle, %dx.types.Handle* @"\01?g_NvidiaExt@@3V?$RWStructuredBuffer@UNvShaderExtnStruct@@@@A", align 4
  %3 = getelementptr inbounds %struct.Payload, %struct.Payload* %payload, i32 0, i32 0
  %4 = load i32, i32* %3, align 4
  %5 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)  ; CreateHandleForLib(Resource)
  %6 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %5, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %7 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %6, i8 1)  ; BufferUpdateCounter(uav,inc)
  %8 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)  ; CreateHandleForLib(Resource)
  %9 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %8, %dx.types.ResourceProperties { i32 4620, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256>
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %9, i32 %7, i32 0, i32 112, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  %10 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)  ; CreateHandleForLib(Resource)
  %11 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %10, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %12 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %11, i8 1)  ; BufferUpdateCounter(uav,inc)
  %13 = icmp ne i32 %12, 0
  %14 = zext i1 %13 to i32
  %15 = call i32 @dx.op.primitiveIndex.i32(i32 161)  ; PrimitiveIndex()
  %16 = call i32 @dx.op.instanceID.i32(i32 141)  ; InstanceID()
  %17 = getelementptr inbounds %struct.BuiltInTriangleIntersectionAttributes, %struct.BuiltInTriangleIntersectionAttributes* %attributes, i32 0, i32 0
  %18 = load <2 x float>, <2 x float>* %17, align 4
  %19 = call float @dx.op.rayTCurrent.f32(i32 154)  ; RayTCurrent()
  %20 = extractelement <2 x float> %18, i32 0
  br i1 %13, label %21, label %59

; <label>:21                                      ; preds = %0
  %22 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)  ; CreateHandleForLib(Resource)
  %23 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %22, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %24 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %23, i8 1)  ; BufferUpdateCounter(uav,inc)
  %25 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)  ; CreateHandleForLib(Resource)
  %26 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %25, %dx.types.ResourceProperties { i32 4620, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256>
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %26, i32 %24, i32 0, i32 104, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  %27 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)  ; CreateHandleForLib(Resource)
  %28 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %27, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %29 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %28, i8 1)  ; BufferUpdateCounter(uav,inc)
  %30 = bitcast i32 %29 to float
  %31 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)  ; CreateHandleForLib(Resource)
  %32 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %31, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %33 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %32, i8 1)  ; BufferUpdateCounter(uav,inc)
  %34 = bitcast i32 %33 to float
  %35 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)  ; CreateHandleForLib(Resource)
  %36 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %35, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %37 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %36, i8 1)  ; BufferUpdateCounter(uav,inc)
  %38 = bitcast i32 %37 to float
  %39 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)  ; CreateHandleForLib(Resource)
  %40 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %39, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %41 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %40, i8 1)  ; BufferUpdateCounter(uav,inc)
  %42 = bitcast i32 %41 to float
  %43 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)  ; CreateHandleForLib(Resource)
  %44 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %43, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %45 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %44, i8 1)  ; BufferUpdateCounter(uav,inc)
  %46 = bitcast i32 %45 to float
  %47 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)  ; CreateHandleForLib(Resource)
  %48 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %47, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %49 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %48, i8 1)  ; BufferUpdateCounter(uav,inc)
  %50 = bitcast i32 %49 to float
  %51 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)  ; CreateHandleForLib(Resource)
  %52 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %51, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %53 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %52, i8 1)  ; BufferUpdateCounter(uav,inc)
  %54 = bitcast i32 %53 to float
  %55 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)  ; CreateHandleForLib(Resource)
  %56 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %55, %dx.types.ResourceProperties { i32 37388, i32 256 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=256, counter>
  %57 = call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %56, i8 1)  ; BufferUpdateCounter(uav,inc)
  %58 = bitcast i32 %57 to float
  br label %59

; <label>:59                                      ; preds = %21, %0
  %60 = phi float [ %46, %21 ], [ 0.000000e+00, %0 ]
  %61 = phi float [ %50, %21 ], [ 0.000000e+00, %0 ]
  %62 = phi float [ %54, %21 ], [ 0.000000e+00, %0 ]
  %63 = phi float [ %58, %21 ], [ 0.000000e+00, %0 ]
  %64 = phi float [ %30, %21 ], [ 0.000000e+00, %0 ]
  %65 = phi float [ %34, %21 ], [ 0.000000e+00, %0 ]
  %66 = phi float [ %38, %21 ], [ 0.000000e+00, %0 ]
  %67 = phi float [ %42, %21 ], [ 0.000000e+00, %0 ]
  %68 = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 0)  ; DispatchRaysIndex(col)
  %69 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %1)  ; CreateHandleForLib(Resource)
  %70 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %69, %dx.types.ResourceProperties { i32 4620, i32 64 })  ; AnnotateHandle(res,props)  resource: RWStructuredBuffer<stride=64>
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %70, i32 %68, i32 0, i32 1, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %70, i32 %68, i32 4, i32 %14, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %70, i32 %68, i32 8, i32 %15, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %70, i32 %68, i32 12, i32 %16, i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  %71 = extractelement <2 x float> %18, i64 0
  %72 = extractelement <2 x float> %18, i64 1
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %70, i32 %68, i32 16, float %71, float %72, float undef, float undef, i8 3, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %70, i32 %68, i32 24, float %19, float undef, float undef, float undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %70, i32 %68, i32 28, float %20, float undef, float undef, float undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %70, i32 %68, i32 32, float %64, float %65, float %66, float %67, i8 15, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %70, i32 %68, i32 48, float %60, float %61, float %62, float %63, i8 15, i32 4)  ; RawBufferStore(uav,index,elementOffset,value0,value1,value2,value3,mask,alignment)
  store i32 %4, i32* %3, align 4
  ret void
}

; Function Attrs: nounwind
declare i32 @dx.op.bufferUpdateCounter(i32, %dx.types.Handle, i8) #0

; Function Attrs: nounwind
declare void @dx.op.rawBufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i8, i32) #0

; Function Attrs: nounwind readnone
declare i32 @dx.op.dispatchRaysIndex.i32(i32, i8) #1

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, i8, i32) #2

; Function Attrs: nounwind readonly
declare %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32, %dx.types.Handle, i32, i32, i8, i32) #2

; Function Attrs: nounwind
declare void @dx.op.rawBufferStore.f32(i32, %dx.types.Handle, i32, i32, float, float, float, float, i8, i32) #0

; Function Attrs: nounwind readnone
declare i32 @dx.op.instanceID.i32(i32) #1

; Function Attrs: nounwind readnone
declare i32 @dx.op.primitiveIndex.i32(i32) #1

; Function Attrs: nounwind readonly
declare float @dx.op.rayTCurrent.f32(i32) #2

; Function Attrs: nounwind readonly
declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32) #2

; Function Attrs: nounwind
declare void @dx.op.traceRay.struct.Payload(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float, %struct.Payload*) #0

; Function Attrs: nounwind readnone
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #1

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32, %dx.types.Handle) #2

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
attributes #2 = { nounwind readonly }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!2}
!dx.shaderModel = !{!3}
!dx.resources = !{!4}
!dx.typeAnnotations = !{!17}
!dx.entryPoints = !{!25, !27, !30, !32}

!0 = !{!"dxcoob 1.9.2602.17 (21d28f727)"}
!1 = !{i32 1, i32 6}
!2 = !{i32 1, i32 9}
!3 = !{!"lib", i32 6, i32 6}
!4 = !{!5, !10, !15, null}
!5 = !{!6, !8}
!6 = !{i32 0, %struct.RaytracingAccelerationStructure* bitcast (%dx.types.Handle* @"\01?Scene@@3URaytracingAccelerationStructure@@A" to %struct.RaytracingAccelerationStructure*), !"Scene", i32 0, i32 0, i32 1, i32 16, i32 0, !7}
!7 = !{i32 0, i32 4}
!8 = !{i32 1, %"class.StructuredBuffer<RayIn>"* bitcast (%dx.types.Handle* @"\01?Rays@@3V?$StructuredBuffer@URayIn@@@@A" to %"class.StructuredBuffer<RayIn>"*), !"Rays", i32 0, i32 1, i32 1, i32 12, i32 0, !9}
!9 = !{i32 1, i32 32}
!10 = !{!11, !13}
!11 = !{i32 0, %"class.RWStructuredBuffer<NvShaderExtnStruct>"* bitcast (%dx.types.Handle* @"\01?g_NvidiaExt@@3V?$RWStructuredBuffer@UNvShaderExtnStruct@@@@A" to %"class.RWStructuredBuffer<NvShaderExtnStruct>"*), !"g_NvidiaExt", i32 99, i32 127, i32 1, i32 12, i1 false, i1 true, i1 false, !12}
!12 = !{i32 1, i32 256}
!13 = !{i32 1, %"class.RWStructuredBuffer<LssOut>"* bitcast (%dx.types.Handle* @"\01?Results@@3V?$RWStructuredBuffer@ULssOut@@@@A" to %"class.RWStructuredBuffer<LssOut>"*), !"Results", i32 0, i32 0, i32 1, i32 12, i1 false, i1 false, i1 false, !14}
!14 = !{i32 1, i32 64}
!15 = !{!16}
!16 = !{i32 0, %Params* bitcast (%dx.types.Handle* @Params to %Params*), !"Params", i32 0, i32 0, i32 1, i32 16, null}
!17 = !{i32 1, void ()* @"\01?RayGen@@YAXXZ", !18, void (%struct.Payload*)* @"\01?Miss@@YAXUPayload@@@Z", !21, void (%struct.Payload*, %struct.BuiltInTriangleIntersectionAttributes*)* @"\01?ClosestHit@@YAXUPayload@@UBuiltInTriangleIntersectionAttributes@@@Z", !23}
!18 = !{!19}
!19 = !{i32 1, !20, !20}
!20 = !{}
!21 = !{!19, !22}
!22 = !{i32 2, !20, !20}
!23 = !{!19, !22, !24}
!24 = !{i32 0, !20, !20}
!25 = !{null, !"", null, !4, !26}
!26 = !{i32 0, i64 16}
!27 = !{void (%struct.Payload*, %struct.BuiltInTriangleIntersectionAttributes*)* @"\01?ClosestHit@@YAXUPayload@@UBuiltInTriangleIntersectionAttributes@@@Z", !"\01?ClosestHit@@YAXUPayload@@UBuiltInTriangleIntersectionAttributes@@@Z", null, null, !28}
!28 = !{i32 8, i32 10, i32 6, i32 4, i32 7, i32 8, i32 5, !29}
!29 = !{i32 0}
!30 = !{void (%struct.Payload*)* @"\01?Miss@@YAXUPayload@@@Z", !"\01?Miss@@YAXUPayload@@@Z", null, null, !31}
!31 = !{i32 8, i32 11, i32 6, i32 4, i32 5, !29}
!32 = !{void ()* @"\01?RayGen@@YAXXZ", !"\01?RayGen@@YAXXZ", null, null, !33}
!33 = !{i32 8, i32 7, i32 5, !29}
