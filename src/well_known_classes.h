/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_SRC_WELL_KNOWN_CLASSES_H_
#define ART_SRC_WELL_KNOWN_CLASSES_H_

#include "jni.h"
#include "mutex.h"

namespace art {

class Class;

// Various classes used in JNI. We cache them so we don't have to keep looking
// them up. Similar to libcore's JniConstants (except there's no overlap, so
// we keep them separate).

struct WellKnownClasses {
  static void InitClasses(JNIEnv* env);
  static void Init(JNIEnv* env);

  static Class* ToClass(jclass global_jclass)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  static jclass com_android_dex_Dex;
  static jclass dalvik_system_PathClassLoader;
  static jclass java_lang_ClassLoader;
  static jclass java_lang_ClassNotFoundException;
  static jclass java_lang_Daemons;
  static jclass java_lang_Error;
  static jclass java_lang_reflect_InvocationHandler;
  static jclass java_lang_reflect_Method;
  static jclass java_lang_reflect_Proxy;
  static jclass java_lang_RuntimeException;
  static jclass java_lang_Thread;
  static jclass java_lang_ThreadGroup;
  static jclass java_lang_ThreadLock;
  static jclass java_lang_Thread$UncaughtExceptionHandler;
  static jclass java_lang_Throwable;
  static jclass java_nio_ReadWriteDirectByteBuffer;
  static jclass org_apache_harmony_dalvik_ddmc_Chunk;
  static jclass org_apache_harmony_dalvik_ddmc_DdmServer;

  static jmethodID com_android_dex_Dex_create;
  static jmethodID java_lang_Boolean_valueOf;
  static jmethodID java_lang_Byte_valueOf;
  static jmethodID java_lang_Character_valueOf;
  static jmethodID java_lang_ClassLoader_loadClass;
  static jmethodID java_lang_ClassNotFoundException_init;
  static jmethodID java_lang_Daemons_requestGC;
  static jmethodID java_lang_Daemons_requestHeapTrim;
  static jmethodID java_lang_Daemons_start;
  static jmethodID java_lang_Double_valueOf;
  static jmethodID java_lang_Float_valueOf;
  static jmethodID java_lang_Integer_valueOf;
  static jmethodID java_lang_Long_valueOf;
  static jmethodID java_lang_ref_FinalizerReference_add;
  static jmethodID java_lang_ref_ReferenceQueue_add;
  static jmethodID java_lang_reflect_InvocationHandler_invoke;
  static jmethodID java_lang_Short_valueOf;
  static jmethodID java_lang_Thread_init;
  static jmethodID java_lang_Thread_run;
  static jmethodID java_lang_Thread$UncaughtExceptionHandler_uncaughtException;
  static jmethodID java_lang_ThreadGroup_removeThread;
  static jmethodID java_nio_ReadWriteDirectByteBuffer_init;
  static jmethodID org_apache_harmony_dalvik_ddmc_DdmServer_broadcast;
  static jmethodID org_apache_harmony_dalvik_ddmc_DdmServer_dispatch;

  static jfieldID java_lang_reflect_Proxy_h;
  static jfieldID java_lang_Thread_daemon;
  static jfieldID java_lang_Thread_group;
  static jfieldID java_lang_Thread_lock;
  static jfieldID java_lang_Thread_name;
  static jfieldID java_lang_Thread_priority;
  static jfieldID java_lang_Thread_uncaughtHandler;
  static jfieldID java_lang_Thread_vmData;
  static jfieldID java_lang_ThreadGroup_mainThreadGroup;
  static jfieldID java_lang_ThreadGroup_name;
  static jfieldID java_lang_ThreadGroup_systemThreadGroup;
  static jfieldID java_lang_ThreadLock_thread;
  static jfieldID java_nio_ReadWriteDirectByteBuffer_capacity;
  static jfieldID java_nio_ReadWriteDirectByteBuffer_effectiveDirectAddress;
  static jfieldID org_apache_harmony_dalvik_ddmc_Chunk_data;
  static jfieldID org_apache_harmony_dalvik_ddmc_Chunk_length;
  static jfieldID org_apache_harmony_dalvik_ddmc_Chunk_offset;
  static jfieldID org_apache_harmony_dalvik_ddmc_Chunk_type;
};

}  // namespace art

#endif  // ART_SRC_WELL_KNOWN_CLASSES_H_
