/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "class_linker.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <deque>
#include <string>
#include <utility>
#include <vector>
#include <map>

#include "base/casts.h"
#include "base/logging.h"
#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "class_linker-inl.h"
#include "compiler_callbacks.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "intern_table.h"
#include "interpreter/interpreter.h"
#include "leb128.h"
#include "oat.h"
#include "oat_file.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/iftable-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/proxy.h"
#include "mirror/stack_trace_element.h"
#include "object_utils.h"
#include "os.h"
#include "runtime.h"
#include "entrypoints/entrypoint_utils.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "sirt_ref.h"
#include "stack_indirect_reference_table.h"
#include "thread.h"
#include "UniquePtr.h"
#include "utils.h"
#include "verifier/method_verifier.h"
#include "well_known_classes.h"

namespace art {

static void ThrowNoClassDefFoundError(const char* fmt, ...)
    __attribute__((__format__(__printf__, 1, 2)))
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
static void ThrowNoClassDefFoundError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Thread* self = Thread::Current();
  ThrowLocation throw_location = self->GetCurrentLocationForThrow();
  self->ThrowNewExceptionV(throw_location, "Ljava/lang/NoClassDefFoundError;", fmt, args);
  va_end(args);
}

static void ThrowEarlierClassFailure(mirror::Class* c)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // The class failed to initialize on a previous attempt, so we want to throw
  // a NoClassDefFoundError (v2 2.17.5).  The exception to this rule is if we
  // failed in verification, in which case v2 5.4.1 says we need to re-throw
  // the previous error.
  if (!Runtime::Current()->IsCompiler()) {  // Give info if this occurs at runtime.
    LOG(INFO) << "Rejecting re-init on previously-failed class " << PrettyClass(c);
  }

  CHECK(c->IsErroneous()) << PrettyClass(c) << " " << c->GetStatus();
  Thread* self = Thread::Current();
  ThrowLocation throw_location = self->GetCurrentLocationForThrow();
  if (c->GetVerifyErrorClass() != NULL) {
    // TODO: change the verifier to store an _instance_, with a useful detail message?
    ClassHelper ve_ch(c->GetVerifyErrorClass());
    self->ThrowNewException(throw_location, ve_ch.GetDescriptor(), PrettyDescriptor(c).c_str());
  } else {
    self->ThrowNewException(throw_location, "Ljava/lang/NoClassDefFoundError;",
                            PrettyDescriptor(c).c_str());
  }
}

static void WrapExceptionInInitializer() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();

  ScopedLocalRef<jthrowable> cause(env, env->ExceptionOccurred());
  CHECK(cause.get() != NULL);

  env->ExceptionClear();
  bool is_error = env->IsInstanceOf(cause.get(), WellKnownClasses::java_lang_Error);
  env->Throw(cause.get());

  // We only wrap non-Error exceptions; an Error can just be used as-is.
  if (!is_error) {
    ThrowLocation throw_location = self->GetCurrentLocationForThrow();
    self->ThrowNewWrappedException(throw_location, "Ljava/lang/ExceptionInInitializerError;", NULL);
  }
}

static size_t Hash(const char* s) {
  // This is the java.lang.String hashcode for convenience, not interoperability.
  size_t hash = 0;
  for (; *s != '\0'; ++s) {
    hash = hash * 31 + *s;
  }
  return hash;
}

const char* ClassLinker::class_roots_descriptors_[] = {
  "Ljava/lang/Class;",
  "Ljava/lang/Object;",
  "[Ljava/lang/Class;",
  "[Ljava/lang/Object;",
  "Ljava/lang/String;",
  "Ljava/lang/DexCache;",
  "Ljava/lang/ref/Reference;",
  "Ljava/lang/reflect/ArtField;",
  "Ljava/lang/reflect/ArtMethod;",
  "Ljava/lang/reflect/Proxy;",
  "[Ljava/lang/String;",
  "[Ljava/lang/reflect/ArtField;",
  "[Ljava/lang/reflect/ArtMethod;",
  "Ljava/lang/ClassLoader;",
  "Ljava/lang/Throwable;",
  "Ljava/lang/ClassNotFoundException;",
  "Ljava/lang/StackTraceElement;",
  "Z",
  "B",
  "C",
  "D",
  "F",
  "I",
  "J",
  "S",
  "V",
  "[Z",
  "[B",
  "[C",
  "[D",
  "[F",
  "[I",
  "[J",
  "[S",
  "[Ljava/lang/StackTraceElement;",
};

ClassLinker::ClassLinker(InternTable* intern_table)
    // dex_lock_ is recursive as it may be used in stack dumping.
    : dex_lock_("ClassLinker dex lock", kDefaultMutexLevel),
      dex_cache_image_class_lookup_required_(false),
      failed_dex_cache_class_lookups_(0),
      class_roots_(nullptr),
      array_iftable_(nullptr),
      find_array_class_cache_next_victim_(0),
      init_done_(false),
      log_new_dex_caches_roots_(false),
      log_new_class_table_roots_(false),
      intern_table_(intern_table),
      portable_resolution_trampoline_(nullptr),
      quick_resolution_trampoline_(nullptr),
      portable_imt_conflict_trampoline_(nullptr),
      quick_imt_conflict_trampoline_(nullptr),
      quick_generic_jni_trampoline_(nullptr),
      quick_to_interpreter_bridge_trampoline_(nullptr) {
  CHECK_EQ(arraysize(class_roots_descriptors_), size_t(kClassRootsMax));
  memset(find_array_class_cache_, 0, kFindArrayCacheSize * sizeof(mirror::Class*));
}

void ClassLinker::InitFromCompiler(const std::vector<const DexFile*>& boot_class_path) {
  VLOG(startup) << "ClassLinker::Init";
  CHECK(Runtime::Current()->IsCompiler());

  CHECK(!init_done_);

  // java_lang_Class comes first, it's needed for AllocClass
  Thread* self = Thread::Current();
  gc::Heap* heap = Runtime::Current()->GetHeap();
  // The GC can't handle an object with a null class since we can't get the size of this object.
  heap->IncrementDisableMovingGC(self);
  SirtRef<mirror::Class> java_lang_Class(self, down_cast<mirror::Class*>(
      heap->AllocNonMovableObject<true>(self, nullptr, sizeof(mirror::ClassClass), VoidFunctor())));
  CHECK(java_lang_Class.get() != NULL);
  mirror::Class::SetClassClass(java_lang_Class.get());
  java_lang_Class->SetClass(java_lang_Class.get());
  if (kUseBakerOrBrooksReadBarrier) {
    java_lang_Class->AssertReadBarrierPointer();
  }
  java_lang_Class->SetClassSize(sizeof(mirror::ClassClass));
  heap->DecrementDisableMovingGC(self);
  // AllocClass(mirror::Class*) can now be used

  // Class[] is used for reflection support.
  SirtRef<mirror::Class> class_array_class(self, AllocClass(self, java_lang_Class.get(), sizeof(mirror::Class)));
  class_array_class->SetComponentType(java_lang_Class.get());

  // java_lang_Object comes next so that object_array_class can be created.
  SirtRef<mirror::Class> java_lang_Object(self, AllocClass(self, java_lang_Class.get(), sizeof(mirror::Class)));
  CHECK(java_lang_Object.get() != NULL);
  // backfill Object as the super class of Class.
  java_lang_Class->SetSuperClass(java_lang_Object.get());
  java_lang_Object->SetStatus(mirror::Class::kStatusLoaded, self);

  // Object[] next to hold class roots.
  SirtRef<mirror::Class> object_array_class(self, AllocClass(self, java_lang_Class.get(), sizeof(mirror::Class)));
  object_array_class->SetComponentType(java_lang_Object.get());

  // Setup the char class to be used for char[].
  SirtRef<mirror::Class> char_class(self, AllocClass(self, java_lang_Class.get(), sizeof(mirror::Class)));

  // Setup the char[] class to be used for String.
  SirtRef<mirror::Class> char_array_class(self, AllocClass(self, java_lang_Class.get(), sizeof(mirror::Class)));
  char_array_class->SetComponentType(char_class.get());
  mirror::CharArray::SetArrayClass(char_array_class.get());

  // Setup String.
  SirtRef<mirror::Class> java_lang_String(self, AllocClass(self, java_lang_Class.get(), sizeof(mirror::StringClass)));
  mirror::String::SetClass(java_lang_String.get());
  java_lang_String->SetObjectSize(sizeof(mirror::String));
  java_lang_String->SetStatus(mirror::Class::kStatusResolved, self);

  // Create storage for root classes, save away our work so far (requires descriptors).
  class_roots_ = mirror::ObjectArray<mirror::Class>::Alloc(self, object_array_class.get(),
                                                           kClassRootsMax);
  CHECK(class_roots_ != NULL);
  SetClassRoot(kJavaLangClass, java_lang_Class.get());
  SetClassRoot(kJavaLangObject, java_lang_Object.get());
  SetClassRoot(kClassArrayClass, class_array_class.get());
  SetClassRoot(kObjectArrayClass, object_array_class.get());
  SetClassRoot(kCharArrayClass, char_array_class.get());
  SetClassRoot(kJavaLangString, java_lang_String.get());

  // Setup the primitive type classes.
  SetClassRoot(kPrimitiveBoolean, CreatePrimitiveClass(self, Primitive::kPrimBoolean));
  SetClassRoot(kPrimitiveByte, CreatePrimitiveClass(self, Primitive::kPrimByte));
  SetClassRoot(kPrimitiveShort, CreatePrimitiveClass(self, Primitive::kPrimShort));
  SetClassRoot(kPrimitiveInt, CreatePrimitiveClass(self, Primitive::kPrimInt));
  SetClassRoot(kPrimitiveLong, CreatePrimitiveClass(self, Primitive::kPrimLong));
  SetClassRoot(kPrimitiveFloat, CreatePrimitiveClass(self, Primitive::kPrimFloat));
  SetClassRoot(kPrimitiveDouble, CreatePrimitiveClass(self, Primitive::kPrimDouble));
  SetClassRoot(kPrimitiveVoid, CreatePrimitiveClass(self, Primitive::kPrimVoid));

  // Create array interface entries to populate once we can load system classes.
  array_iftable_ = AllocIfTable(self, 2);

  // Create int array type for AllocDexCache (done in AppendToBootClassPath).
  SirtRef<mirror::Class> int_array_class(self, AllocClass(self, java_lang_Class.get(), sizeof(mirror::Class)));
  int_array_class->SetComponentType(GetClassRoot(kPrimitiveInt));
  mirror::IntArray::SetArrayClass(int_array_class.get());
  SetClassRoot(kIntArrayClass, int_array_class.get());

  // now that these are registered, we can use AllocClass() and AllocObjectArray

  // Set up DexCache. This cannot be done later since AppendToBootClassPath calls AllocDexCache.
  SirtRef<mirror::Class>
      java_lang_DexCache(self, AllocClass(self, java_lang_Class.get(), sizeof(mirror::DexCacheClass)));
  SetClassRoot(kJavaLangDexCache, java_lang_DexCache.get());
  java_lang_DexCache->SetObjectSize(sizeof(mirror::DexCache));
  java_lang_DexCache->SetStatus(mirror::Class::kStatusResolved, self);

  // Constructor, Field, Method, and AbstractMethod are necessary so that FindClass can link members.
  SirtRef<mirror::Class> java_lang_reflect_ArtField(self, AllocClass(self, java_lang_Class.get(),
                                                                     sizeof(mirror::ArtFieldClass)));
  CHECK(java_lang_reflect_ArtField.get() != NULL);
  java_lang_reflect_ArtField->SetObjectSize(sizeof(mirror::ArtField));
  SetClassRoot(kJavaLangReflectArtField, java_lang_reflect_ArtField.get());
  java_lang_reflect_ArtField->SetStatus(mirror::Class::kStatusResolved, self);
  mirror::ArtField::SetClass(java_lang_reflect_ArtField.get());

  SirtRef<mirror::Class> java_lang_reflect_ArtMethod(self, AllocClass(self, java_lang_Class.get(),
                                                                      sizeof(mirror::ArtMethodClass)));
  CHECK(java_lang_reflect_ArtMethod.get() != NULL);
  java_lang_reflect_ArtMethod->SetObjectSize(sizeof(mirror::ArtMethod));
  SetClassRoot(kJavaLangReflectArtMethod, java_lang_reflect_ArtMethod.get());
  java_lang_reflect_ArtMethod->SetStatus(mirror::Class::kStatusResolved, self);

  mirror::ArtMethod::SetClass(java_lang_reflect_ArtMethod.get());

  // Set up array classes for string, field, method
  SirtRef<mirror::Class> object_array_string(self, AllocClass(self, java_lang_Class.get(),
                                                              sizeof(mirror::Class)));
  object_array_string->SetComponentType(java_lang_String.get());
  SetClassRoot(kJavaLangStringArrayClass, object_array_string.get());

  SirtRef<mirror::Class> object_array_art_method(self, AllocClass(self, java_lang_Class.get(),
                                                                  sizeof(mirror::Class)));
  object_array_art_method->SetComponentType(java_lang_reflect_ArtMethod.get());
  SetClassRoot(kJavaLangReflectArtMethodArrayClass, object_array_art_method.get());

  SirtRef<mirror::Class> object_array_art_field(self, AllocClass(self, java_lang_Class.get(),
                                                                 sizeof(mirror::Class)));
  object_array_art_field->SetComponentType(java_lang_reflect_ArtField.get());
  SetClassRoot(kJavaLangReflectArtFieldArrayClass, object_array_art_field.get());

  // Setup boot_class_path_ and register class_path now that we can use AllocObjectArray to create
  // DexCache instances. Needs to be after String, Field, Method arrays since AllocDexCache uses
  // these roots.
  CHECK_NE(0U, boot_class_path.size());
  for (size_t i = 0; i != boot_class_path.size(); ++i) {
    const DexFile* dex_file = boot_class_path[i];
    CHECK(dex_file != NULL);
    AppendToBootClassPath(*dex_file);
  }

  // now we can use FindSystemClass

  // run char class through InitializePrimitiveClass to finish init
  InitializePrimitiveClass(char_class.get(), Primitive::kPrimChar);
  SetClassRoot(kPrimitiveChar, char_class.get());  // needs descriptor

  // Create runtime resolution and imt conflict methods. Also setup the default imt.
  Runtime* runtime = Runtime::Current();
  runtime->SetResolutionMethod(runtime->CreateResolutionMethod());
  runtime->SetImtConflictMethod(runtime->CreateImtConflictMethod());
  runtime->SetDefaultImt(runtime->CreateDefaultImt(this));

  // Object, String and DexCache need to be rerun through FindSystemClass to finish init
  java_lang_Object->SetStatus(mirror::Class::kStatusNotReady, self);
  mirror::Class* Object_class = FindSystemClass(self, "Ljava/lang/Object;");
  CHECK_EQ(java_lang_Object.get(), Object_class);
  CHECK_EQ(java_lang_Object->GetObjectSize(), sizeof(mirror::Object));
  java_lang_String->SetStatus(mirror::Class::kStatusNotReady, self);
  mirror::Class* String_class = FindSystemClass(self, "Ljava/lang/String;");
  CHECK_EQ(java_lang_String.get(), String_class);
  CHECK_EQ(java_lang_String->GetObjectSize(), sizeof(mirror::String));
  java_lang_DexCache->SetStatus(mirror::Class::kStatusNotReady, self);
  mirror::Class* DexCache_class = FindSystemClass(self, "Ljava/lang/DexCache;");
  CHECK_EQ(java_lang_String.get(), String_class);
  CHECK_EQ(java_lang_DexCache.get(), DexCache_class);
  CHECK_EQ(java_lang_DexCache->GetObjectSize(), sizeof(mirror::DexCache));

  // Setup the primitive array type classes - can't be done until Object has a vtable.
  SetClassRoot(kBooleanArrayClass, FindSystemClass(self, "[Z"));
  mirror::BooleanArray::SetArrayClass(GetClassRoot(kBooleanArrayClass));

  SetClassRoot(kByteArrayClass, FindSystemClass(self, "[B"));
  mirror::ByteArray::SetArrayClass(GetClassRoot(kByteArrayClass));

  mirror::Class* found_char_array_class = FindSystemClass(self, "[C");
  CHECK_EQ(char_array_class.get(), found_char_array_class);

  SetClassRoot(kShortArrayClass, FindSystemClass(self, "[S"));
  mirror::ShortArray::SetArrayClass(GetClassRoot(kShortArrayClass));

  mirror::Class* found_int_array_class = FindSystemClass(self, "[I");
  CHECK_EQ(int_array_class.get(), found_int_array_class);

  SetClassRoot(kLongArrayClass, FindSystemClass(self, "[J"));
  mirror::LongArray::SetArrayClass(GetClassRoot(kLongArrayClass));

  SetClassRoot(kFloatArrayClass, FindSystemClass(self, "[F"));
  mirror::FloatArray::SetArrayClass(GetClassRoot(kFloatArrayClass));

  SetClassRoot(kDoubleArrayClass, FindSystemClass(self, "[D"));
  mirror::DoubleArray::SetArrayClass(GetClassRoot(kDoubleArrayClass));

  mirror::Class* found_class_array_class = FindSystemClass(self, "[Ljava/lang/Class;");
  CHECK_EQ(class_array_class.get(), found_class_array_class);

  mirror::Class* found_object_array_class = FindSystemClass(self, "[Ljava/lang/Object;");
  CHECK_EQ(object_array_class.get(), found_object_array_class);

  // Setup the single, global copy of "iftable".
  mirror::Class* java_lang_Cloneable = FindSystemClass(self, "Ljava/lang/Cloneable;");
  CHECK(java_lang_Cloneable != NULL);
  mirror::Class* java_io_Serializable = FindSystemClass(self, "Ljava/io/Serializable;");
  CHECK(java_io_Serializable != NULL);
  // We assume that Cloneable/Serializable don't have superinterfaces -- normally we'd have to
  // crawl up and explicitly list all of the supers as well.
  array_iftable_->SetInterface(0, java_lang_Cloneable);
  array_iftable_->SetInterface(1, java_io_Serializable);

  // Sanity check Class[] and Object[]'s interfaces.
  ClassHelper kh(class_array_class.get());
  CHECK_EQ(java_lang_Cloneable, kh.GetDirectInterface(0));
  CHECK_EQ(java_io_Serializable, kh.GetDirectInterface(1));
  kh.ChangeClass(object_array_class.get());
  CHECK_EQ(java_lang_Cloneable, kh.GetDirectInterface(0));
  CHECK_EQ(java_io_Serializable, kh.GetDirectInterface(1));
  // Run Class, ArtField, and ArtMethod through FindSystemClass. This initializes their
  // dex_cache_ fields and register them in class_table_.
  mirror::Class* Class_class = FindSystemClass(self, "Ljava/lang/Class;");
  CHECK_EQ(java_lang_Class.get(), Class_class);

  java_lang_reflect_ArtMethod->SetStatus(mirror::Class::kStatusNotReady, self);
  mirror::Class* Art_method_class = FindSystemClass(self, "Ljava/lang/reflect/ArtMethod;");
  CHECK_EQ(java_lang_reflect_ArtMethod.get(), Art_method_class);

  java_lang_reflect_ArtField->SetStatus(mirror::Class::kStatusNotReady, self);
  mirror::Class* Art_field_class = FindSystemClass(self, "Ljava/lang/reflect/ArtField;");
  CHECK_EQ(java_lang_reflect_ArtField.get(), Art_field_class);

  mirror::Class* String_array_class = FindSystemClass(self, class_roots_descriptors_[kJavaLangStringArrayClass]);
  CHECK_EQ(object_array_string.get(), String_array_class);

  mirror::Class* Art_method_array_class =
      FindSystemClass(self, class_roots_descriptors_[kJavaLangReflectArtMethodArrayClass]);
  CHECK_EQ(object_array_art_method.get(), Art_method_array_class);

  mirror::Class* Art_field_array_class =
      FindSystemClass(self, class_roots_descriptors_[kJavaLangReflectArtFieldArrayClass]);
  CHECK_EQ(object_array_art_field.get(), Art_field_array_class);

  // End of special init trickery, subsequent classes may be loaded via FindSystemClass.

  // Create java.lang.reflect.Proxy root.
  mirror::Class* java_lang_reflect_Proxy = FindSystemClass(self, "Ljava/lang/reflect/Proxy;");
  SetClassRoot(kJavaLangReflectProxy, java_lang_reflect_Proxy);

  // java.lang.ref classes need to be specially flagged, but otherwise are normal classes
  mirror::Class* java_lang_ref_Reference = FindSystemClass(self, "Ljava/lang/ref/Reference;");
  SetClassRoot(kJavaLangRefReference, java_lang_ref_Reference);
  mirror::Class* java_lang_ref_FinalizerReference = FindSystemClass(self, "Ljava/lang/ref/FinalizerReference;");
  java_lang_ref_FinalizerReference->SetAccessFlags(
      java_lang_ref_FinalizerReference->GetAccessFlags() |
          kAccClassIsReference | kAccClassIsFinalizerReference);
  mirror::Class* java_lang_ref_PhantomReference = FindSystemClass(self, "Ljava/lang/ref/PhantomReference;");
  java_lang_ref_PhantomReference->SetAccessFlags(
      java_lang_ref_PhantomReference->GetAccessFlags() |
          kAccClassIsReference | kAccClassIsPhantomReference);
  mirror::Class* java_lang_ref_SoftReference = FindSystemClass(self, "Ljava/lang/ref/SoftReference;");
  java_lang_ref_SoftReference->SetAccessFlags(
      java_lang_ref_SoftReference->GetAccessFlags() | kAccClassIsReference);
  mirror::Class* java_lang_ref_WeakReference = FindSystemClass(self, "Ljava/lang/ref/WeakReference;");
  java_lang_ref_WeakReference->SetAccessFlags(
      java_lang_ref_WeakReference->GetAccessFlags() |
          kAccClassIsReference | kAccClassIsWeakReference);

  // Setup the ClassLoader, verifying the object_size_.
  mirror::Class* java_lang_ClassLoader = FindSystemClass(self, "Ljava/lang/ClassLoader;");
  CHECK_EQ(java_lang_ClassLoader->GetObjectSize(), sizeof(mirror::ClassLoader));
  SetClassRoot(kJavaLangClassLoader, java_lang_ClassLoader);

  // Set up java.lang.Throwable, java.lang.ClassNotFoundException, and
  // java.lang.StackTraceElement as a convenience.
  SetClassRoot(kJavaLangThrowable, FindSystemClass(self, "Ljava/lang/Throwable;"));
  mirror::Throwable::SetClass(GetClassRoot(kJavaLangThrowable));
  SetClassRoot(kJavaLangClassNotFoundException, FindSystemClass(self, "Ljava/lang/ClassNotFoundException;"));
  SetClassRoot(kJavaLangStackTraceElement, FindSystemClass(self, "Ljava/lang/StackTraceElement;"));
  SetClassRoot(kJavaLangStackTraceElementArrayClass, FindSystemClass(self, "[Ljava/lang/StackTraceElement;"));
  mirror::StackTraceElement::SetClass(GetClassRoot(kJavaLangStackTraceElement));

  FinishInit(self);

  VLOG(startup) << "ClassLinker::InitFromCompiler exiting";
}

void ClassLinker::FinishInit(Thread* self) {
  VLOG(startup) << "ClassLinker::FinishInit entering";

  // Let the heap know some key offsets into java.lang.ref instances
  // Note: we hard code the field indexes here rather than using FindInstanceField
  // as the types of the field can't be resolved prior to the runtime being
  // fully initialized
  mirror::Class* java_lang_ref_Reference = GetClassRoot(kJavaLangRefReference);
  mirror::Class* java_lang_ref_FinalizerReference =
      FindSystemClass(self, "Ljava/lang/ref/FinalizerReference;");

  mirror::ArtField* pendingNext = java_lang_ref_Reference->GetInstanceField(0);
  FieldHelper fh(pendingNext);
  CHECK_STREQ(fh.GetName(), "pendingNext");
  CHECK_STREQ(fh.GetTypeDescriptor(), "Ljava/lang/ref/Reference;");

  mirror::ArtField* queue = java_lang_ref_Reference->GetInstanceField(1);
  fh.ChangeField(queue);
  CHECK_STREQ(fh.GetName(), "queue");
  CHECK_STREQ(fh.GetTypeDescriptor(), "Ljava/lang/ref/ReferenceQueue;");

  mirror::ArtField* queueNext = java_lang_ref_Reference->GetInstanceField(2);
  fh.ChangeField(queueNext);
  CHECK_STREQ(fh.GetName(), "queueNext");
  CHECK_STREQ(fh.GetTypeDescriptor(), "Ljava/lang/ref/Reference;");

  mirror::ArtField* referent = java_lang_ref_Reference->GetInstanceField(3);
  fh.ChangeField(referent);
  CHECK_STREQ(fh.GetName(), "referent");
  CHECK_STREQ(fh.GetTypeDescriptor(), "Ljava/lang/Object;");

  mirror::ArtField* zombie = java_lang_ref_FinalizerReference->GetInstanceField(2);
  fh.ChangeField(zombie);
  CHECK_STREQ(fh.GetName(), "zombie");
  CHECK_STREQ(fh.GetTypeDescriptor(), "Ljava/lang/Object;");

  // ensure all class_roots_ are initialized
  for (size_t i = 0; i < kClassRootsMax; i++) {
    ClassRoot class_root = static_cast<ClassRoot>(i);
    mirror::Class* klass = GetClassRoot(class_root);
    CHECK(klass != NULL);
    DCHECK(klass->IsArrayClass() || klass->IsPrimitive() || klass->GetDexCache() != NULL);
    // note SetClassRoot does additional validation.
    // if possible add new checks there to catch errors early
  }

  CHECK(array_iftable_ != NULL);

  // disable the slow paths in FindClass and CreatePrimitiveClass now
  // that Object, Class, and Object[] are setup
  init_done_ = true;

  VLOG(startup) << "ClassLinker::FinishInit exiting";
}

void ClassLinker::RunRootClinits() {
  Thread* self = Thread::Current();
  for (size_t i = 0; i < ClassLinker::kClassRootsMax; ++i) {
    mirror::Class* c = GetClassRoot(ClassRoot(i));
    if (!c->IsArrayClass() && !c->IsPrimitive()) {
      SirtRef<mirror::Class> sirt_class(self, GetClassRoot(ClassRoot(i)));
      EnsureInitialized(sirt_class, true, true);
      self->AssertNoPendingException();
    }
  }
}

bool ClassLinker::GenerateOatFile(const char* dex_filename,
                                  int oat_fd,
                                  const char* oat_cache_filename,
                                  std::string* error_msg) {
  Locks::mutator_lock_->AssertNotHeld(Thread::Current());  // Avoid starving GC.
  std::string dex2oat(GetAndroidRoot());
  dex2oat += (kIsDebugBuild ? "/bin/dex2oatd" : "/bin/dex2oat");

  gc::Heap* heap = Runtime::Current()->GetHeap();
  std::string boot_image_option("--boot-image=");
  boot_image_option += heap->GetImageSpace()->GetImageFilename();

  std::string dex_file_option("--dex-file=");
  dex_file_option += dex_filename;

  std::string oat_fd_option("--oat-fd=");
  StringAppendF(&oat_fd_option, "%d", oat_fd);

  std::string oat_location_option("--oat-location=");
  oat_location_option += oat_cache_filename;

  std::vector<std::string> argv;
  argv.push_back(dex2oat);
  argv.push_back("--runtime-arg");
  argv.push_back("-Xms64m");
  argv.push_back("--runtime-arg");
  argv.push_back("-Xmx64m");
  argv.push_back("--runtime-arg");
  argv.push_back("-classpath");
  argv.push_back("--runtime-arg");
  argv.push_back(Runtime::Current()->GetClassPathString());

  Runtime::Current()->AddCurrentRuntimeFeaturesAsDex2OatArguments(&argv);

  if (!Runtime::Current()->IsVerificationEnabled()) {
    argv.push_back("--compiler-filter=verify-none");
  }

  if (!kIsTargetBuild) {
    argv.push_back("--host");
  }

  argv.push_back(boot_image_option);
  argv.push_back(dex_file_option);
  argv.push_back(oat_fd_option);
  argv.push_back(oat_location_option);
  const std::vector<std::string>& compiler_options = Runtime::Current()->GetCompilerOptions();
  for (size_t i = 0; i < compiler_options.size(); ++i) {
    argv.push_back(compiler_options[i].c_str());
  }

  return Exec(argv, error_msg);
}

const OatFile* ClassLinker::RegisterOatFile(const OatFile* oat_file) {
  WriterMutexLock mu(Thread::Current(), dex_lock_);
  if (kIsDebugBuild) {
    for (size_t i = 0; i < oat_files_.size(); ++i) {
      CHECK_NE(oat_file, oat_files_[i]) << oat_file->GetLocation();
    }
  }
  VLOG(class_linker) << "Registering " << oat_file->GetLocation();
  oat_files_.push_back(oat_file);
  return oat_file;
}

OatFile& ClassLinker::GetImageOatFile(gc::space::ImageSpace* space) {
  VLOG(startup) << "ClassLinker::GetImageOatFile entering";
  OatFile* oat_file = space->ReleaseOatFile();
  CHECK_EQ(RegisterOatFile(oat_file), oat_file);
  VLOG(startup) << "ClassLinker::GetImageOatFile exiting";
  return *oat_file;
}

const OatFile* ClassLinker::FindOpenedOatFileForDexFile(const DexFile& dex_file) {
  const char* dex_location = dex_file.GetLocation().c_str();
  uint32_t dex_location_checksum = dex_file.GetLocationChecksum();
  return FindOpenedOatFileFromDexLocation(dex_location, &dex_location_checksum);
}

const OatFile* ClassLinker::FindOpenedOatFileFromDexLocation(const char* dex_location,
                                                             const uint32_t* const dex_location_checksum) {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  for (size_t i = 0; i < oat_files_.size(); i++) {
    const OatFile* oat_file = oat_files_[i];
    DCHECK(oat_file != NULL);
    const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location,
                                                                      dex_location_checksum,
                                                                      false);
    if (oat_dex_file != NULL) {
      return oat_file;
    }
  }
  return NULL;
}

const DexFile* ClassLinker::FindDexFileInOatLocation(const char* dex_location,
                                                     uint32_t dex_location_checksum,
                                                     const char* oat_location,
                                                     std::string* error_msg) {
  UniquePtr<OatFile> oat_file(OatFile::Open(oat_location, oat_location, NULL,
                                            !Runtime::Current()->IsCompiler(),
                                            error_msg));
  if (oat_file.get() == nullptr) {
    *error_msg = StringPrintf("Failed to find existing oat file at %s: %s", oat_location,
                              error_msg->c_str());
    return nullptr;
  }
  Runtime* runtime = Runtime::Current();
  const ImageHeader& image_header = runtime->GetHeap()->GetImageSpace()->GetImageHeader();
  uint32_t expected_image_oat_checksum = image_header.GetOatChecksum();
  uint32_t actual_image_oat_checksum = oat_file->GetOatHeader().GetImageFileLocationOatChecksum();
  if (expected_image_oat_checksum != actual_image_oat_checksum) {
    *error_msg = StringPrintf("Failed to find oat file at '%s' with expected image oat checksum of "
                              "0x%x, found 0x%x", oat_location, expected_image_oat_checksum,
                              actual_image_oat_checksum);
    return nullptr;
  }

  uintptr_t expected_image_oat_offset = reinterpret_cast<uintptr_t>(image_header.GetOatDataBegin());
  uint32_t actual_image_oat_offset = oat_file->GetOatHeader().GetImageFileLocationOatDataBegin();
  if (expected_image_oat_offset != actual_image_oat_offset) {
    *error_msg = StringPrintf("Failed to find oat file at '%s' with expected image oat offset %"
                              PRIuPTR ", found %ud", oat_location, expected_image_oat_offset,
                              actual_image_oat_offset);
    return nullptr;
  }
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location,
                                                                    &dex_location_checksum);
  if (oat_dex_file == nullptr) {
    *error_msg = StringPrintf("Failed to find oat file at '%s' containing '%s'", oat_location,
                              dex_location);
    return nullptr;
  }
  uint32_t expected_dex_checksum = dex_location_checksum;
  uint32_t actual_dex_checksum = oat_dex_file->GetDexFileLocationChecksum();
  if (expected_dex_checksum != actual_dex_checksum) {
    *error_msg = StringPrintf("Failed to find oat file at '%s' with expected dex checksum of 0x%x, "
                              "found 0x%x", oat_location, expected_dex_checksum,
                              actual_dex_checksum);
    return nullptr;
  }
  const DexFile* dex_file = oat_dex_file->OpenDexFile(error_msg);
  if (dex_file != nullptr) {
    RegisterOatFile(oat_file.release());
  }
  return dex_file;
}

class ScopedFlock {
 public:
  ScopedFlock() {}

  bool Init(const char* filename, std::string* error_msg) {
    while (true) {
      file_.reset(OS::OpenFileWithFlags(filename, O_CREAT | O_RDWR));
      if (file_.get() == NULL) {
        *error_msg = StringPrintf("Failed to open file '%s': %s", filename, strerror(errno));
        return false;
      }
      int flock_result = TEMP_FAILURE_RETRY(flock(file_->Fd(), LOCK_EX));
      if (flock_result != 0) {
        *error_msg = StringPrintf("Failed to lock file '%s': %s", filename, strerror(errno));
        return false;
      }
      struct stat fstat_stat;
      int fstat_result = TEMP_FAILURE_RETRY(fstat(file_->Fd(), &fstat_stat));
      if (fstat_result != 0) {
        *error_msg = StringPrintf("Failed to fstat file '%s': %s", filename, strerror(errno));
        return false;
      }
      struct stat stat_stat;
      int stat_result = TEMP_FAILURE_RETRY(stat(filename, &stat_stat));
      if (stat_result != 0) {
        PLOG(WARNING) << "Failed to stat, will retry: " << filename;
        // ENOENT can happen if someone racing with us unlinks the file we created so just retry.
        continue;
      }
      if (fstat_stat.st_dev != stat_stat.st_dev || fstat_stat.st_ino != stat_stat.st_ino) {
        LOG(WARNING) << "File changed while locking, will retry: " << filename;
        continue;
      }
      return true;
    }
  }

  File& GetFile() {
    return *file_;
  }

  ~ScopedFlock() {
    if (file_.get() != NULL) {
      int flock_result = TEMP_FAILURE_RETRY(flock(file_->Fd(), LOCK_UN));
      CHECK_EQ(0, flock_result);
    }
  }

 private:
  UniquePtr<File> file_;

  DISALLOW_COPY_AND_ASSIGN(ScopedFlock);
};

const DexFile* ClassLinker::FindOrCreateOatFileForDexLocation(const char* dex_location,
                                                              uint32_t dex_location_checksum,
                                                              const char* oat_location,
                                                              std::vector<std::string>* error_msgs) {
  // We play a locking game here so that if two different processes
  // race to generate (or worse, one tries to open a partial generated
  // file) we will be okay. This is actually common with apps that use
  // DexClassLoader to work around the dex method reference limit and
  // that have a background service running in a separate process.
  ScopedFlock scoped_flock;
  std::string error_msg;
  if (!scoped_flock.Init(oat_location, &error_msg)) {
    error_msgs->push_back(error_msg);
    return nullptr;
  }

  // Check if we already have an up-to-date output file
  const DexFile* dex_file = FindDexFileInOatLocation(dex_location, dex_location_checksum,
                                                     oat_location, &error_msg);
  if (dex_file != nullptr) {
    return dex_file;
  }
  std::string compound_msg = StringPrintf("Failed to find dex file '%s' in oat location '%s': %s",
                                          dex_location, oat_location, error_msg.c_str());
  VLOG(class_linker) << compound_msg;
  error_msgs->push_back(compound_msg);

  // Generate the output oat file for the dex file
  VLOG(class_linker) << "Generating oat file " << oat_location << " for " << dex_location;
  if (!GenerateOatFile(dex_location, scoped_flock.GetFile().Fd(), oat_location, &error_msg)) {
    CHECK(!error_msg.empty());
    error_msgs->push_back(error_msg);
    return nullptr;
  }
  UniquePtr<OatFile> oat_file(OatFile::Open(oat_location, oat_location, NULL,
                                            !Runtime::Current()->IsCompiler(),
                                            &error_msg));
  if (oat_file.get() == nullptr) {
    compound_msg = StringPrintf("\nFailed to open generated oat file '%s': %s",
                                oat_location, error_msg.c_str());
    error_msgs->push_back(compound_msg);
    return nullptr;
  }
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location,
                                                                    &dex_location_checksum);
  if (oat_dex_file == nullptr) {
    error_msg = StringPrintf("\nFailed to find dex file '%s' (checksum 0x%x) in generated out file "
                             "'%s'", dex_location, dex_location_checksum, oat_location);
    error_msgs->push_back(error_msg);
    return nullptr;
  }
  const DexFile* result = oat_dex_file->OpenDexFile(&error_msg);
  CHECK(result != nullptr) << error_msgs << ", " << error_msg;
  CHECK_EQ(dex_location_checksum, result->GetLocationChecksum())
          << "dex_location=" << dex_location << " oat_location=" << oat_location << std::hex
          << " dex_location_checksum=" << dex_location_checksum
          << " DexFile::GetLocationChecksum()=" << result->GetLocationChecksum();
  RegisterOatFile(oat_file.release());
  return result;
}

bool ClassLinker::VerifyOatFileChecksums(const OatFile* oat_file,
                                         const char* dex_location,
                                         uint32_t dex_location_checksum,
                                         const InstructionSet instruction_set,
                                         std::string* error_msg) {
  Runtime* runtime = Runtime::Current();
  const gc::space::ImageSpace* image_space = runtime->GetHeap()->GetImageSpace();

  // If the requested instruction set is the same as the current runtime,
  // we can use the checksums directly. If it isn't, we'll have to read the
  // image header from the image for the right instruction set.
  uint32_t image_oat_checksum = 0;
  uintptr_t image_oat_data_begin = 0;
  if (instruction_set == kRuntimeISA) {
    const ImageHeader& image_header = image_space->GetImageHeader();
    image_oat_checksum = image_header.GetOatChecksum();
    image_oat_data_begin = reinterpret_cast<uintptr_t>(image_header.GetOatDataBegin());
  } else {
    UniquePtr<ImageHeader> image_header(gc::space::ImageSpace::ReadImageHeaderOrDie(
        image_space->GetImageLocation().c_str(), instruction_set));
    image_oat_checksum = image_header->GetOatChecksum();
    image_oat_data_begin = reinterpret_cast<uintptr_t>(image_header->GetOatDataBegin());
  }
  const OatHeader& oat_header = oat_file->GetOatHeader();
  bool image_check = ((oat_header.GetImageFileLocationOatChecksum() == image_oat_checksum)
                      && (oat_header.GetImageFileLocationOatDataBegin() == image_oat_data_begin));

  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location, &dex_location_checksum);
  if (oat_dex_file == NULL) {
    *error_msg = StringPrintf("oat file '%s' does not contain contents for '%s' with checksum 0x%x",
                              oat_file->GetLocation().c_str(), dex_location, dex_location_checksum);
    std::vector<const OatFile::OatDexFile*> oat_dex_files = oat_file->GetOatDexFiles();
    for (size_t i = 0; i < oat_dex_files.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files[i];
      *error_msg  += StringPrintf("\noat file '%s' contains contents for '%s'",
                                  oat_file->GetLocation().c_str(),
                                  oat_dex_file->GetDexFileLocation().c_str());
    }
    return false;
  }
  bool dex_check = dex_location_checksum == oat_dex_file->GetDexFileLocationChecksum();

  if (image_check && dex_check) {
    return true;
  }

  if (!image_check) {
    ScopedObjectAccess soa(Thread::Current());
    *error_msg = StringPrintf("oat file '%s' mismatch (0x%x, %d) with (0x%x, %" PRIdPTR ")",
                              oat_file->GetLocation().c_str(),
                              oat_file->GetOatHeader().GetImageFileLocationOatChecksum(),
                              oat_file->GetOatHeader().GetImageFileLocationOatDataBegin(),
                              image_oat_checksum, image_oat_data_begin);
  }
  if (!dex_check) {
    *error_msg = StringPrintf("oat file '%s' mismatch (0x%x) with '%s' (0x%x)",
                              oat_file->GetLocation().c_str(),
                              oat_dex_file->GetDexFileLocationChecksum(),
                              dex_location, dex_location_checksum);
  }
  return false;
}

const DexFile* ClassLinker::VerifyAndOpenDexFileFromOatFile(const std::string& oat_file_location,
                                                            const char* dex_location,
                                                            std::string* error_msg,
                                                            bool* open_failed) {
  UniquePtr<const OatFile> oat_file(FindOatFileFromOatLocation(oat_file_location, error_msg));
  if (oat_file.get() == nullptr) {
    *open_failed = true;
    return nullptr;
  }
  *open_failed = false;
  const DexFile* dex_file = nullptr;
  uint32_t dex_location_checksum;
  if (!DexFile::GetChecksum(dex_location, &dex_location_checksum, error_msg)) {
    // If no classes.dex found in dex_location, it has been stripped or is corrupt, assume oat is
    // up-to-date. This is the common case in user builds for jar's and apk's in the /system
    // directory.
    const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_location, NULL);
    if (oat_dex_file == nullptr) {
      *error_msg = StringPrintf("Dex checksum mismatch for location '%s' and failed to find oat "
                                "dex file '%s': %s", oat_file_location.c_str(), dex_location,
                                error_msg->c_str());
      return nullptr;
    }
    dex_file = oat_dex_file->OpenDexFile(error_msg);
  } else {
    bool verified = VerifyOatFileChecksums(oat_file.get(), dex_location, dex_location_checksum,
                                           kRuntimeISA, error_msg);
    if (!verified) {
      return nullptr;
    }
    dex_file = oat_file->GetOatDexFile(dex_location,
                                       &dex_location_checksum)->OpenDexFile(error_msg);
  }
  if (dex_file != nullptr) {
    RegisterOatFile(oat_file.release());
  }
  return dex_file;
}

const DexFile* ClassLinker::FindDexFileInOatFileFromDexLocation(const char* dex_location,
                                                                const uint32_t* const dex_location_checksum,
                                                                std::vector<std::string>* error_msgs) {
  const OatFile* open_oat_file = FindOpenedOatFileFromDexLocation(dex_location,
                                                                  dex_location_checksum);
  if (open_oat_file != nullptr) {
    const OatFile::OatDexFile* oat_dex_file = open_oat_file->GetOatDexFile(dex_location,
                                                                           dex_location_checksum);
    std::string error_msg;
    const DexFile* ret = oat_dex_file->OpenDexFile(&error_msg);
    if (ret == nullptr) {
      error_msgs->push_back(error_msg);
    }
    return ret;
  }

  // Look for an existing file next to dex. for example, for
  // /foo/bar/baz.jar, look for /foo/bar/baz.odex.
  std::string odex_filename(OatFile::DexFilenameToOdexFilename(dex_location));
  bool open_failed;
  std::string error_msg;
  const DexFile* dex_file = VerifyAndOpenDexFileFromOatFile(odex_filename, dex_location,
                                                            &error_msg, &open_failed);
  if (dex_file != nullptr) {
    return dex_file;
  }
  if (dex_location_checksum == nullptr) {
    error_msgs->push_back(StringPrintf("Failed to open oat file from %s and no classes.dex found in"
                                      "%s: %s", odex_filename.c_str(), dex_location,
                                       error_msg.c_str()));
    return nullptr;
  }

  std::string cache_error_msg;
  const std::string dalvik_cache(GetDalvikCacheOrDie(GetInstructionSetString(kRuntimeISA)));
  std::string cache_location(GetDalvikCacheFilenameOrDie(dex_location,
                                                         dalvik_cache.c_str()));
  dex_file = VerifyAndOpenDexFileFromOatFile(cache_location, dex_location, &cache_error_msg,
                                             &open_failed);
  if (dex_file != nullptr) {
    return dex_file;
  }
  if (!open_failed && TEMP_FAILURE_RETRY(unlink(cache_location.c_str())) != 0) {
    PLOG(FATAL) << "Failed to remove obsolete oat file from " << cache_location;
  }
  std::string compound_msg = StringPrintf("Failed to open oat file from %s (error '%s') or %s "
                                          "(error '%s').", odex_filename.c_str(), error_msg.c_str(),
                                          cache_location.c_str(), cache_error_msg.c_str());
  VLOG(class_linker) << compound_msg;
  error_msgs->push_back(compound_msg);

  // Try to generate oat file if it wasn't found or was obsolete.
  return FindOrCreateOatFileForDexLocation(dex_location, *dex_location_checksum,
                                           cache_location.c_str(), error_msgs);
}

const OatFile* ClassLinker::FindOpenedOatFileFromOatLocation(const std::string& oat_location) {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  for (size_t i = 0; i < oat_files_.size(); i++) {
    const OatFile* oat_file = oat_files_[i];
    DCHECK(oat_file != nullptr);
    if (oat_file->GetLocation() == oat_location) {
      return oat_file;
    }
  }
  return nullptr;
}

const OatFile* ClassLinker::FindOatFileFromOatLocation(const std::string& oat_location,
                                                       std::string* error_msg) {
  const OatFile* oat_file = FindOpenedOatFileFromOatLocation(oat_location);
  if (oat_file != nullptr) {
    return oat_file;
  }

  oat_file = OatFile::Open(oat_location, oat_location, NULL, !Runtime::Current()->IsCompiler(),
                           error_msg);
  if (oat_file == NULL) {
    return NULL;
  }
  return oat_file;
}

static void InitFromImageInterpretOnlyCallback(mirror::Object* obj, void* arg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ClassLinker* class_linker = reinterpret_cast<ClassLinker*>(arg);

  DCHECK(obj != NULL);
  DCHECK(class_linker != NULL);

  if (obj->IsArtMethod()) {
    mirror::ArtMethod* method = obj->AsArtMethod();
    if (!method->IsNative()) {
      method->SetEntryPointFromInterpreter(interpreter::artInterpreterToInterpreterBridge);
      if (method != Runtime::Current()->GetResolutionMethod()) {
        method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
        method->SetEntryPointFromPortableCompiledCode(GetPortableToInterpreterBridge());
      }
    }
  }
}

void ClassLinker::InitFromImage() {
  VLOG(startup) << "ClassLinker::InitFromImage entering";
  CHECK(!init_done_);

  Thread* self = Thread::Current();
  gc::Heap* heap = Runtime::Current()->GetHeap();
  gc::space::ImageSpace* space = heap->GetImageSpace();
  dex_cache_image_class_lookup_required_ = true;
  CHECK(space != NULL);
  OatFile& oat_file = GetImageOatFile(space);
  CHECK_EQ(oat_file.GetOatHeader().GetImageFileLocationOatChecksum(), 0U);
  CHECK_EQ(oat_file.GetOatHeader().GetImageFileLocationOatDataBegin(), 0U);
  CHECK(oat_file.GetOatHeader().GetImageFileLocation().empty());
  portable_resolution_trampoline_ = oat_file.GetOatHeader().GetPortableResolutionTrampoline();
  quick_resolution_trampoline_ = oat_file.GetOatHeader().GetQuickResolutionTrampoline();
  portable_imt_conflict_trampoline_ = oat_file.GetOatHeader().GetPortableImtConflictTrampoline();
  quick_imt_conflict_trampoline_ = oat_file.GetOatHeader().GetQuickImtConflictTrampoline();
  quick_generic_jni_trampoline_ = oat_file.GetOatHeader().GetQuickGenericJniTrampoline();
  quick_to_interpreter_bridge_trampoline_ = oat_file.GetOatHeader().GetQuickToInterpreterBridge();
  mirror::Object* dex_caches_object = space->GetImageHeader().GetImageRoot(ImageHeader::kDexCaches);
  mirror::ObjectArray<mirror::DexCache>* dex_caches =
      dex_caches_object->AsObjectArray<mirror::DexCache>();

  SirtRef<mirror::ObjectArray<mirror::Class> > class_roots(
      self,
      space->GetImageHeader().GetImageRoot(ImageHeader::kClassRoots)->AsObjectArray<mirror::Class>());
  class_roots_ = class_roots.get();

  // Special case of setting up the String class early so that we can test arbitrary objects
  // as being Strings or not
  mirror::String::SetClass(GetClassRoot(kJavaLangString));

  CHECK_EQ(oat_file.GetOatHeader().GetDexFileCount(),
           static_cast<uint32_t>(dex_caches->GetLength()));
  for (int32_t i = 0; i < dex_caches->GetLength(); i++) {
    SirtRef<mirror::DexCache> dex_cache(self, dex_caches->Get(i));
    const std::string& dex_file_location(dex_cache->GetLocation()->ToModifiedUtf8());
    const OatFile::OatDexFile* oat_dex_file = oat_file.GetOatDexFile(dex_file_location.c_str(),
                                                                     nullptr);
    CHECK(oat_dex_file != NULL) << oat_file.GetLocation() << " " << dex_file_location;
    std::string error_msg;
    const DexFile* dex_file = oat_dex_file->OpenDexFile(&error_msg);
    if (dex_file == NULL) {
      LOG(FATAL) << "Failed to open dex file " << dex_file_location
                 << " from within oat file " << oat_file.GetLocation()
                 << " error '" << error_msg << "'";
    }

    CHECK_EQ(dex_file->GetLocationChecksum(), oat_dex_file->GetDexFileLocationChecksum());

    AppendToBootClassPath(*dex_file, dex_cache);
  }

  // Set classes on AbstractMethod early so that IsMethod tests can be performed during the live
  // bitmap walk.
  mirror::ArtMethod::SetClass(GetClassRoot(kJavaLangReflectArtMethod));

  // Set entry point to interpreter if in InterpretOnly mode.
  if (Runtime::Current()->GetInstrumentation()->InterpretOnly()) {
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    heap->VisitObjects(InitFromImageInterpretOnlyCallback, this);
  }

  // reinit class_roots_
  mirror::Class::SetClassClass(class_roots->Get(kJavaLangClass));
  class_roots_ = class_roots.get();

  // reinit array_iftable_ from any array class instance, they should be ==
  array_iftable_ = GetClassRoot(kObjectArrayClass)->GetIfTable();
  DCHECK(array_iftable_ == GetClassRoot(kBooleanArrayClass)->GetIfTable());
  // String class root was set above
  mirror::ArtField::SetClass(GetClassRoot(kJavaLangReflectArtField));
  mirror::BooleanArray::SetArrayClass(GetClassRoot(kBooleanArrayClass));
  mirror::ByteArray::SetArrayClass(GetClassRoot(kByteArrayClass));
  mirror::CharArray::SetArrayClass(GetClassRoot(kCharArrayClass));
  mirror::DoubleArray::SetArrayClass(GetClassRoot(kDoubleArrayClass));
  mirror::FloatArray::SetArrayClass(GetClassRoot(kFloatArrayClass));
  mirror::IntArray::SetArrayClass(GetClassRoot(kIntArrayClass));
  mirror::LongArray::SetArrayClass(GetClassRoot(kLongArrayClass));
  mirror::ShortArray::SetArrayClass(GetClassRoot(kShortArrayClass));
  mirror::Throwable::SetClass(GetClassRoot(kJavaLangThrowable));
  mirror::StackTraceElement::SetClass(GetClassRoot(kJavaLangStackTraceElement));

  FinishInit(self);

  VLOG(startup) << "ClassLinker::InitFromImage exiting";
}

// Keep in sync with InitCallback. Anything we visit, we need to
// reinit references to when reinitializing a ClassLinker from a
// mapped image.
void ClassLinker::VisitRoots(RootCallback* callback, void* arg, VisitRootFlags flags) {
  callback(reinterpret_cast<mirror::Object**>(&class_roots_), arg, 0, kRootVMInternal);
  Thread* self = Thread::Current();
  {
    ReaderMutexLock mu(self, dex_lock_);
    if ((flags & kVisitRootFlagAllRoots) != 0) {
      for (auto dex_cache : dex_caches_) {
        callback(reinterpret_cast<mirror::Object**>(&dex_cache), arg, 0, kRootVMInternal);
      }
    } else if ((flags & kVisitRootFlagNewRoots) != 0) {
      for (auto index : new_dex_cache_roots_) {
        callback(reinterpret_cast<mirror::Object**>(&dex_caches_[index]), arg, 0, kRootVMInternal);
      }
    }
    if ((flags & kVisitRootFlagClearRootLog) != 0) {
      new_dex_cache_roots_.clear();
    }
    if ((flags & kVisitRootFlagStartLoggingNewRoots) != 0) {
      log_new_dex_caches_roots_ = true;
    } else if ((flags & kVisitRootFlagStopLoggingNewRoots) != 0) {
      log_new_dex_caches_roots_ = false;
    }
  }
  {
    WriterMutexLock mu(self, *Locks::classlinker_classes_lock_);
    if ((flags & kVisitRootFlagAllRoots) != 0) {
      for (std::pair<const size_t, mirror::Class*>& it : class_table_) {
        callback(reinterpret_cast<mirror::Object**>(&it.second), arg, 0, kRootStickyClass);
      }
    } else if ((flags & kVisitRootFlagNewRoots) != 0) {
      for (auto& pair : new_class_roots_) {
        mirror::Object* old_ref = pair.second;
        callback(reinterpret_cast<mirror::Object**>(&pair.second), arg, 0, kRootStickyClass);
        if (UNLIKELY(pair.second != old_ref)) {
          // Uh ohes, GC moved a root in the log. Need to search the class_table and update the
          // corresponding object. This is slow, but luckily for us, this may only happen with a
          // concurrent moving GC.
          for (auto it = class_table_.lower_bound(pair.first), end = class_table_.end();
              it != end && it->first == pair.first; ++it) {
            // If the class stored matches the old class, update it to the new value.
            if (old_ref == it->second) {
              it->second = pair.second;
            }
          }
        }
      }
    }
    if ((flags & kVisitRootFlagClearRootLog) != 0) {
      new_class_roots_.clear();
    }
    if ((flags & kVisitRootFlagStartLoggingNewRoots) != 0) {
      log_new_class_table_roots_ = true;
    } else if ((flags & kVisitRootFlagStopLoggingNewRoots) != 0) {
      log_new_class_table_roots_ = false;
    }
    // We deliberately ignore the class roots in the image since we
    // handle image roots by using the MS/CMS rescanning of dirty cards.
  }
  callback(reinterpret_cast<mirror::Object**>(&array_iftable_), arg, 0, kRootVMInternal);
  DCHECK(array_iftable_ != nullptr);
  for (size_t i = 0; i < kFindArrayCacheSize; ++i) {
    if (find_array_class_cache_[i] != nullptr) {
      callback(reinterpret_cast<mirror::Object**>(&find_array_class_cache_[i]), arg, 0,
               kRootVMInternal);
    }
  }
}

void ClassLinker::VisitClasses(ClassVisitor* visitor, void* arg) {
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  for (const std::pair<size_t, mirror::Class*>& it : class_table_) {
    if (!visitor(it.second, arg)) {
      return;
    }
  }
}

static bool GetClassesVisitor(mirror::Class* c, void* arg) {
  std::set<mirror::Class*>* classes = reinterpret_cast<std::set<mirror::Class*>*>(arg);
  classes->insert(c);
  return true;
}

void ClassLinker::VisitClassesWithoutClassesLock(ClassVisitor* visitor, void* arg) {
  std::set<mirror::Class*> classes;
  VisitClasses(GetClassesVisitor, &classes);
  for (mirror::Class* klass : classes) {
    if (!visitor(klass, arg)) {
      return;
    }
  }
}

ClassLinker::~ClassLinker() {
  mirror::Class::ResetClass();
  mirror::String::ResetClass();
  mirror::ArtField::ResetClass();
  mirror::ArtMethod::ResetClass();
  mirror::BooleanArray::ResetArrayClass();
  mirror::ByteArray::ResetArrayClass();
  mirror::CharArray::ResetArrayClass();
  mirror::DoubleArray::ResetArrayClass();
  mirror::FloatArray::ResetArrayClass();
  mirror::IntArray::ResetArrayClass();
  mirror::LongArray::ResetArrayClass();
  mirror::ShortArray::ResetArrayClass();
  mirror::Throwable::ResetClass();
  mirror::StackTraceElement::ResetClass();
  STLDeleteElements(&boot_class_path_);
  STLDeleteElements(&oat_files_);
}

mirror::DexCache* ClassLinker::AllocDexCache(Thread* self, const DexFile& dex_file) {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  SirtRef<mirror::Class> dex_cache_class(self, GetClassRoot(kJavaLangDexCache));
  SirtRef<mirror::DexCache> dex_cache(
      self, down_cast<mirror::DexCache*>(
          heap->AllocObject<true>(self, dex_cache_class.get(), dex_cache_class->GetObjectSize(),
                                  VoidFunctor())));
  if (dex_cache.get() == NULL) {
    return NULL;
  }
  SirtRef<mirror::String>
      location(self, intern_table_->InternStrong(dex_file.GetLocation().c_str()));
  if (location.get() == NULL) {
    return NULL;
  }
  SirtRef<mirror::ObjectArray<mirror::String> >
      strings(self, AllocStringArray(self, dex_file.NumStringIds()));
  if (strings.get() == NULL) {
    return NULL;
  }
  SirtRef<mirror::ObjectArray<mirror::Class> >
      types(self, AllocClassArray(self, dex_file.NumTypeIds()));
  if (types.get() == NULL) {
    return NULL;
  }
  SirtRef<mirror::ObjectArray<mirror::ArtMethod> >
      methods(self, AllocArtMethodArray(self, dex_file.NumMethodIds()));
  if (methods.get() == NULL) {
    return NULL;
  }
  SirtRef<mirror::ObjectArray<mirror::ArtField> >
      fields(self, AllocArtFieldArray(self, dex_file.NumFieldIds()));
  if (fields.get() == NULL) {
    return NULL;
  }
  dex_cache->Init(&dex_file, location.get(), strings.get(), types.get(), methods.get(),
                  fields.get());
  return dex_cache.get();
}

// Used to initialize a class in the allocation code path to ensure it is guarded by a StoreStore
// fence.
class InitializeClassVisitor {
 public:
  explicit InitializeClassVisitor(uint32_t class_size) : class_size_(class_size) {
  }

  void operator()(mirror::Object* obj, size_t usable_size) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK_LE(class_size_, usable_size);
    // Avoid AsClass as object is not yet in live bitmap or allocation stack.
    mirror::Class* klass = down_cast<mirror::Class*>(obj);
    // DCHECK(klass->IsClass());
    klass->SetClassSize(class_size_);
    klass->SetPrimitiveType(Primitive::kPrimNot);  // Default to not being primitive.
    klass->SetDexClassDefIndex(DexFile::kDexNoIndex16);  // Default to no valid class def index.
    klass->SetDexTypeIndex(DexFile::kDexNoIndex16);  // Default to no valid type index.
  }

 private:
  const uint32_t class_size_;

  DISALLOW_COPY_AND_ASSIGN(InitializeClassVisitor);
};

mirror::Class* ClassLinker::AllocClass(Thread* self, mirror::Class* java_lang_Class,
                                       uint32_t class_size) {
  DCHECK_GE(class_size, sizeof(mirror::Class));
  gc::Heap* heap = Runtime::Current()->GetHeap();
  InitializeClassVisitor visitor(class_size);
  mirror::Object* k =
      kMovingClasses ? heap->AllocObject<true>(self, java_lang_Class, class_size, visitor)
                     : heap->AllocNonMovableObject<true>(self, java_lang_Class, class_size, visitor);
  if (UNLIKELY(k == nullptr)) {
    CHECK(self->IsExceptionPending());  // OOME.
    return nullptr;
  }
  return k->AsClass();
}

mirror::Class* ClassLinker::AllocClass(Thread* self, uint32_t class_size) {
  return AllocClass(self, GetClassRoot(kJavaLangClass), class_size);
}

mirror::ArtField* ClassLinker::AllocArtField(Thread* self) {
  return down_cast<mirror::ArtField*>(
      GetClassRoot(kJavaLangReflectArtField)->AllocNonMovableObject(self));
}

mirror::ArtMethod* ClassLinker::AllocArtMethod(Thread* self) {
  return down_cast<mirror::ArtMethod*>(
      GetClassRoot(kJavaLangReflectArtMethod)->AllocNonMovableObject(self));
}

mirror::ObjectArray<mirror::StackTraceElement>* ClassLinker::AllocStackTraceElementArray(
    Thread* self, size_t length) {
  return mirror::ObjectArray<mirror::StackTraceElement>::Alloc(
      self, GetClassRoot(kJavaLangStackTraceElementArrayClass), length);
}

static mirror::Class* EnsureResolved(Thread* self, mirror::Class* klass)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(klass != NULL);
  // Wait for the class if it has not already been linked.
  if (!klass->IsResolved() && !klass->IsErroneous()) {
    SirtRef<mirror::Class> sirt_class(self, klass);
    ObjectLock<mirror::Class> lock(self, &sirt_class);
    // Check for circular dependencies between classes.
    if (!sirt_class->IsResolved() && sirt_class->GetClinitThreadId() == self->GetTid()) {
      ThrowClassCircularityError(sirt_class.get());
      sirt_class->SetStatus(mirror::Class::kStatusError, self);
      return nullptr;
    }
    // Wait for the pending initialization to complete.
    while (!sirt_class->IsResolved() && !sirt_class->IsErroneous()) {
      lock.WaitIgnoringInterrupts();
    }
    klass = sirt_class.get();
  }
  if (klass->IsErroneous()) {
    ThrowEarlierClassFailure(klass);
    return nullptr;
  }
  // Return the loaded class.  No exceptions should be pending.
  CHECK(klass->IsResolved()) << PrettyClass(klass);
  self->AssertNoPendingException();
  return klass;
}

mirror::Class* ClassLinker::FindClass(Thread* self, const char* descriptor,
                                      const SirtRef<mirror::ClassLoader>& class_loader) {
  DCHECK_NE(*descriptor, '\0') << "descriptor is empty string";
  DCHECK(self != nullptr);
  self->AssertNoPendingException();
  if (descriptor[1] == '\0') {
    // only the descriptors of primitive types should be 1 character long, also avoid class lookup
    // for primitive classes that aren't backed by dex files.
    return FindPrimitiveClass(descriptor[0]);
  }
  // Find the class in the loaded classes table.
  mirror::Class* klass = LookupClass(descriptor, class_loader.get());
  if (klass != NULL) {
    return EnsureResolved(self, klass);
  }
  // Class is not yet loaded.
  if (descriptor[0] == '[') {
    return CreateArrayClass(self, descriptor, class_loader);
  } else if (class_loader.get() == nullptr) {
    DexFile::ClassPathEntry pair = DexFile::FindInClassPath(descriptor, boot_class_path_);
    if (pair.second != NULL) {
      SirtRef<mirror::ClassLoader> class_loader(self, nullptr);
      return DefineClass(descriptor, class_loader, *pair.first, *pair.second);
    }
  } else if (Runtime::Current()->UseCompileTimeClassPath()) {
    // First try the boot class path, we check the descriptor first to avoid an unnecessary
    // throw of a NoClassDefFoundError.
    if (IsInBootClassPath(descriptor)) {
      mirror::Class* system_class = FindSystemClass(self, descriptor);
      CHECK(system_class != NULL);
      return system_class;
    }
    // Next try the compile time class path.
    const std::vector<const DexFile*>* class_path;
    {
      ScopedObjectAccessUnchecked soa(self);
      ScopedLocalRef<jobject> jclass_loader(soa.Env(),
                                            soa.AddLocalReference<jobject>(class_loader.get()));
      class_path = &Runtime::Current()->GetCompileTimeClassPath(jclass_loader.get());
    }

    DexFile::ClassPathEntry pair = DexFile::FindInClassPath(descriptor, *class_path);
    if (pair.second != NULL) {
      return DefineClass(descriptor, class_loader, *pair.first, *pair.second);
    }

  } else {
    ScopedObjectAccessUnchecked soa(self);
    ScopedLocalRef<jobject> class_loader_object(soa.Env(),
                                                soa.AddLocalReference<jobject>(class_loader.get()));
    std::string class_name_string(DescriptorToDot(descriptor));
    ScopedLocalRef<jobject> result(soa.Env(), NULL);
    {
      ScopedThreadStateChange tsc(self, kNative);
      ScopedLocalRef<jobject> class_name_object(soa.Env(),
                                                soa.Env()->NewStringUTF(class_name_string.c_str()));
      if (class_name_object.get() == NULL) {
        return NULL;
      }
      CHECK(class_loader_object.get() != NULL);
      result.reset(soa.Env()->CallObjectMethod(class_loader_object.get(),
                                               WellKnownClasses::java_lang_ClassLoader_loadClass,
                                               class_name_object.get()));
    }
    if (self->IsExceptionPending()) {
      // If the ClassLoader threw, pass that exception up.
      return NULL;
    } else if (result.get() == NULL) {
      // broken loader - throw NPE to be compatible with Dalvik
      ThrowNullPointerException(NULL, StringPrintf("ClassLoader.loadClass returned null for %s",
                                                   class_name_string.c_str()).c_str());
      return NULL;
    } else {
      // success, return mirror::Class*
      return soa.Decode<mirror::Class*>(result.get());
    }
  }

  ThrowNoClassDefFoundError("Class %s not found", PrintableString(descriptor).c_str());
  return NULL;
}

mirror::Class* ClassLinker::DefineClass(const char* descriptor,
                                        const SirtRef<mirror::ClassLoader>& class_loader,
                                        const DexFile& dex_file,
                                        const DexFile::ClassDef& dex_class_def) {
  Thread* self = Thread::Current();
  SirtRef<mirror::Class> klass(self, NULL);
  // Load the class from the dex file.
  if (UNLIKELY(!init_done_)) {
    // finish up init of hand crafted class_roots_
    if (strcmp(descriptor, "Ljava/lang/Object;") == 0) {
      klass.reset(GetClassRoot(kJavaLangObject));
    } else if (strcmp(descriptor, "Ljava/lang/Class;") == 0) {
      klass.reset(GetClassRoot(kJavaLangClass));
    } else if (strcmp(descriptor, "Ljava/lang/String;") == 0) {
      klass.reset(GetClassRoot(kJavaLangString));
    } else if (strcmp(descriptor, "Ljava/lang/DexCache;") == 0) {
      klass.reset(GetClassRoot(kJavaLangDexCache));
    } else if (strcmp(descriptor, "Ljava/lang/reflect/ArtField;") == 0) {
      klass.reset(GetClassRoot(kJavaLangReflectArtField));
    } else if (strcmp(descriptor, "Ljava/lang/reflect/ArtMethod;") == 0) {
      klass.reset(GetClassRoot(kJavaLangReflectArtMethod));
    } else {
      klass.reset(AllocClass(self, SizeOfClass(dex_file, dex_class_def)));
    }
  } else {
    klass.reset(AllocClass(self, SizeOfClass(dex_file, dex_class_def)));
  }
  if (UNLIKELY(klass.get() == NULL)) {
    CHECK(self->IsExceptionPending());  // Expect an OOME.
    return NULL;
  }
  klass->SetDexCache(FindDexCache(dex_file));
  LoadClass(dex_file, dex_class_def, klass, class_loader.get());
  // Check for a pending exception during load
  if (self->IsExceptionPending()) {
    klass->SetStatus(mirror::Class::kStatusError, self);
    return NULL;
  }
  ObjectLock<mirror::Class> lock(self, &klass);
  klass->SetClinitThreadId(self->GetTid());
  // Add the newly loaded class to the loaded classes table.
  mirror::Class* existing = InsertClass(descriptor, klass.get(), Hash(descriptor));
  if (existing != NULL) {
    // We failed to insert because we raced with another thread. Calling EnsureResolved may cause
    // this thread to block.
    return EnsureResolved(self, existing);
  }
  // Finish loading (if necessary) by finding parents
  CHECK(!klass->IsLoaded());
  if (!LoadSuperAndInterfaces(klass, dex_file)) {
    // Loading failed.
    klass->SetStatus(mirror::Class::kStatusError, self);
    return NULL;
  }
  CHECK(klass->IsLoaded());
  // Link the class (if necessary)
  CHECK(!klass->IsResolved());
  // TODO: Use fast jobjects?
  SirtRef<mirror::ObjectArray<mirror::Class> > interfaces(self, nullptr);
  if (!LinkClass(self, klass, interfaces)) {
    // Linking failed.
    klass->SetStatus(mirror::Class::kStatusError, self);
    return NULL;
  }
  CHECK(klass->IsResolved());

  /*
   * We send CLASS_PREPARE events to the debugger from here.  The
   * definition of "preparation" is creating the static fields for a
   * class and initializing them to the standard default values, but not
   * executing any code (that comes later, during "initialization").
   *
   * We did the static preparation in LinkClass.
   *
   * The class has been prepared and resolved but possibly not yet verified
   * at this point.
   */
  Dbg::PostClassPrepare(klass.get());

  return klass.get();
}

// Precomputes size that will be needed for Class, matching LinkStaticFields
uint32_t ClassLinker::SizeOfClass(const DexFile& dex_file,
                                const DexFile::ClassDef& dex_class_def) {
  const byte* class_data = dex_file.GetClassData(dex_class_def);
  size_t num_ref = 0;
  size_t num_32 = 0;
  size_t num_64 = 0;
  if (class_data != NULL) {
    for (ClassDataItemIterator it(dex_file, class_data); it.HasNextStaticField(); it.Next()) {
      const DexFile::FieldId& field_id = dex_file.GetFieldId(it.GetMemberIndex());
      const char* descriptor = dex_file.GetFieldTypeDescriptor(field_id);
      char c = descriptor[0];
      if (c == 'L' || c == '[') {
        num_ref++;
      } else if (c == 'J' || c == 'D') {
        num_64++;
      } else {
        num_32++;
      }
    }
  }
  // start with generic class data
  uint32_t size = sizeof(mirror::Class);
  // follow with reference fields which must be contiguous at start
  size += (num_ref * sizeof(uint32_t));
  // if there are 64-bit fields to add, make sure they are aligned
  if (num_64 != 0 && size != RoundUp(size, 8)) {  // for 64-bit alignment
    if (num_32 != 0) {
      // use an available 32-bit field for padding
      num_32--;
    }
    size += sizeof(uint32_t);  // either way, we are adding a word
    DCHECK_EQ(size, RoundUp(size, 8));
  }
  // tack on any 64-bit fields now that alignment is assured
  size += (num_64 * sizeof(uint64_t));
  // tack on any remaining 32-bit fields
  size += (num_32 * sizeof(uint32_t));
  return size;
}

OatFile::OatClass ClassLinker::GetOatClass(const DexFile& dex_file, uint16_t class_def_idx) {
  DCHECK_NE(class_def_idx, DexFile::kDexNoIndex16);
  const OatFile* oat_file = FindOpenedOatFileForDexFile(dex_file);
  CHECK(oat_file != NULL) << dex_file.GetLocation();
  uint dex_location_checksum = dex_file.GetLocationChecksum();
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file.GetLocation().c_str(),
                                                                    &dex_location_checksum);
  CHECK(oat_dex_file != NULL) << dex_file.GetLocation();
  return oat_dex_file->GetOatClass(class_def_idx);
}

static uint32_t GetOatMethodIndexFromMethodIndex(const DexFile& dex_file, uint16_t class_def_idx,
                                                 uint32_t method_idx) {
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_idx);
  const byte* class_data = dex_file.GetClassData(class_def);
  CHECK(class_data != NULL);
  ClassDataItemIterator it(dex_file, class_data);
  // Skip fields
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
  // Process methods
  size_t class_def_method_index = 0;
  while (it.HasNextDirectMethod()) {
    if (it.GetMemberIndex() == method_idx) {
      return class_def_method_index;
    }
    class_def_method_index++;
    it.Next();
  }
  while (it.HasNextVirtualMethod()) {
    if (it.GetMemberIndex() == method_idx) {
      return class_def_method_index;
    }
    class_def_method_index++;
    it.Next();
  }
  DCHECK(!it.HasNext());
  LOG(FATAL) << "Failed to find method index " << method_idx << " in " << dex_file.GetLocation();
  return 0;
}

const OatFile::OatMethod ClassLinker::GetOatMethodFor(mirror::ArtMethod* method) {
  // Although we overwrite the trampoline of non-static methods, we may get here via the resolution
  // method for direct methods (or virtual methods made direct).
  mirror::Class* declaring_class = method->GetDeclaringClass();
  size_t oat_method_index;
  if (method->IsStatic() || method->IsDirect()) {
    // Simple case where the oat method index was stashed at load time.
    oat_method_index = method->GetMethodIndex();
  } else {
    // We're invoking a virtual method directly (thanks to sharpening), compute the oat_method_index
    // by search for its position in the declared virtual methods.
    oat_method_index = declaring_class->NumDirectMethods();
    size_t end = declaring_class->NumVirtualMethods();
    bool found = false;
    for (size_t i = 0; i < end; i++) {
      if (declaring_class->GetVirtualMethod(i) == method) {
        found = true;
        break;
      }
      oat_method_index++;
    }
    CHECK(found) << "Didn't find oat method index for virtual method: " << PrettyMethod(method);
  }
  DCHECK_EQ(oat_method_index,
            GetOatMethodIndexFromMethodIndex(*declaring_class->GetDexCache()->GetDexFile(),
                                             method->GetDeclaringClass()->GetDexClassDefIndex(),
                                             method->GetDexMethodIndex()));
  const OatFile::OatClass oat_class = GetOatClass(*declaring_class->GetDexCache()->GetDexFile(),
                                                  declaring_class->GetDexClassDefIndex());

  return oat_class.GetOatMethod(oat_method_index);
}

// Special case to get oat code without overwriting a trampoline.
const void* ClassLinker::GetQuickOatCodeFor(mirror::ArtMethod* method) {
  CHECK(!method->IsAbstract()) << PrettyMethod(method);
  if (method->IsProxyMethod()) {
    return GetQuickProxyInvokeHandler();
  }
  const void* result = GetOatMethodFor(method).GetQuickCode();
  if (result == nullptr) {
    if (method->IsNative()) {
      // No code and native? Use generic trampoline.
      result = GetQuickGenericJniTrampoline();
    } else if (method->IsPortableCompiled()) {
      // No code? Do we expect portable code?
      result = GetQuickToPortableBridge();
    } else {
      // No code? You must mean to go into the interpreter.
      result = GetQuickToInterpreterBridge();
    }
  }
  return result;
}

const void* ClassLinker::GetPortableOatCodeFor(mirror::ArtMethod* method,
                                               bool* have_portable_code) {
  CHECK(!method->IsAbstract()) << PrettyMethod(method);
  *have_portable_code = false;
  if (method->IsProxyMethod()) {
    return GetPortableProxyInvokeHandler();
  }
  const OatFile::OatMethod oat_method = GetOatMethodFor(method);
  const void* result = oat_method.GetPortableCode();
  if (result == nullptr) {
    if (oat_method.GetQuickCode() == nullptr) {
      // No code? You must mean to go into the interpreter.
      result = GetPortableToInterpreterBridge();
    } else {
      // No code? But there's quick code, so use a bridge.
      result = GetPortableToQuickBridge();
    }
  } else {
    *have_portable_code = true;
  }
  return result;
}

const void* ClassLinker::GetQuickOatCodeFor(const DexFile& dex_file, uint16_t class_def_idx,
                                            uint32_t method_idx) {
  const OatFile::OatClass oat_class = GetOatClass(dex_file, class_def_idx);
  uint32_t oat_method_idx = GetOatMethodIndexFromMethodIndex(dex_file, class_def_idx, method_idx);
  return oat_class.GetOatMethod(oat_method_idx).GetQuickCode();
}

const void* ClassLinker::GetPortableOatCodeFor(const DexFile& dex_file, uint16_t class_def_idx,
                                               uint32_t method_idx) {
  const OatFile::OatClass oat_class = GetOatClass(dex_file, class_def_idx);
  uint32_t oat_method_idx = GetOatMethodIndexFromMethodIndex(dex_file, class_def_idx, method_idx);
  return oat_class.GetOatMethod(oat_method_idx).GetPortableCode();
}

// Returns true if the method must run with interpreter, false otherwise.
static bool NeedsInterpreter(mirror::ArtMethod* method, const void* quick_code,
                             const void* portable_code) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if ((quick_code == nullptr) && (portable_code == nullptr)) {
    // No code: need interpreter.
    // May return true for native code, in the case of generic JNI
    // DCHECK(!method->IsNative());
    return true;
  }
#ifdef ART_SEA_IR_MODE
  ScopedObjectAccess soa(Thread::Current());
  if (std::string::npos != PrettyMethod(method).find("fibonacci")) {
    LOG(INFO) << "Found " << PrettyMethod(method);
    return false;
  }
#endif
  // If interpreter mode is enabled, every method (except native and proxy) must
  // be run with interpreter.
  return Runtime::Current()->GetInstrumentation()->InterpretOnly() &&
         !method->IsNative() && !method->IsProxyMethod();
}

void ClassLinker::FixupStaticTrampolines(mirror::Class* klass) {
  DCHECK(klass->IsInitialized()) << PrettyDescriptor(klass);
  if (klass->NumDirectMethods() == 0) {
    return;  // No direct methods => no static methods.
  }
  Runtime* runtime = Runtime::Current();
  if (!runtime->IsStarted() || runtime->UseCompileTimeClassPath()) {
    return;  // OAT file unavailable.
  }
  ClassHelper kh(klass);
  const DexFile& dex_file = kh.GetDexFile();
  const DexFile::ClassDef* dex_class_def = kh.GetClassDef();
  CHECK(dex_class_def != nullptr);
  const byte* class_data = dex_file.GetClassData(*dex_class_def);
  // There should always be class data if there were direct methods.
  CHECK(class_data != nullptr) << PrettyDescriptor(klass);
  const OatFile::OatClass oat_class = GetOatClass(dex_file, klass->GetDexClassDefIndex());
  ClassDataItemIterator it(dex_file, class_data);
  // Skip fields
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
  // Link the code of methods skipped by LinkCode.
  for (size_t method_index = 0; it.HasNextDirectMethod(); ++method_index, it.Next()) {
    mirror::ArtMethod* method = klass->GetDirectMethod(method_index);
    if (!method->IsStatic()) {
      // Only update static methods.
      continue;
    }
    const void* portable_code = oat_class.GetOatMethod(method_index).GetPortableCode();
    const void* quick_code = oat_class.GetOatMethod(method_index).GetQuickCode();
    const bool enter_interpreter = NeedsInterpreter(method, quick_code, portable_code);
    bool have_portable_code = false;
    if (enter_interpreter) {
      // Use interpreter entry point.
      // Check whether the method is native, in which case it's generic JNI.
      if (quick_code == nullptr && portable_code == nullptr && method->IsNative()) {
        quick_code = GetQuickGenericJniTrampoline();
        portable_code = GetPortableToQuickBridge();
      } else {
        portable_code = GetPortableToInterpreterBridge();
        quick_code = GetQuickToInterpreterBridge();
      }
    } else {
      if (portable_code == nullptr) {
        portable_code = GetPortableToQuickBridge();
      } else {
        have_portable_code = true;
      }
      if (quick_code == nullptr) {
        quick_code = GetQuickToPortableBridge();
      }
    }
    runtime->GetInstrumentation()->UpdateMethodsCode(method, quick_code, portable_code,
                                                     have_portable_code);
  }
  // Ignore virtual methods on the iterator.
}

static void LinkCode(const SirtRef<mirror::ArtMethod>& method, const OatFile::OatClass* oat_class,
                     const DexFile& dex_file, uint32_t dex_method_index, uint32_t method_index)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Method shouldn't have already been linked.
  DCHECK(method->GetEntryPointFromQuickCompiledCode() == nullptr);
  DCHECK(method->GetEntryPointFromPortableCompiledCode() == nullptr);
  // Every kind of method should at least get an invoke stub from the oat_method.
  // non-abstract methods also get their code pointers.
  const OatFile::OatMethod oat_method = oat_class->GetOatMethod(method_index);
  oat_method.LinkMethod(method.get());

  // Install entry point from interpreter.
  Runtime* runtime = Runtime::Current();
  bool enter_interpreter = NeedsInterpreter(method.get(),
                                            method->GetEntryPointFromQuickCompiledCode(),
                                            method->GetEntryPointFromPortableCompiledCode());
  if (enter_interpreter && !method->IsNative()) {
    method->SetEntryPointFromInterpreter(interpreter::artInterpreterToInterpreterBridge);
  } else {
    method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
  }

  if (method->IsAbstract()) {
    method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
    method->SetEntryPointFromPortableCompiledCode(GetPortableToInterpreterBridge());
    return;
  }

  bool have_portable_code = false;
  if (method->IsStatic() && !method->IsConstructor()) {
    // For static methods excluding the class initializer, install the trampoline.
    // It will be replaced by the proper entry point by ClassLinker::FixupStaticTrampolines
    // after initializing class (see ClassLinker::InitializeClass method).
    method->SetEntryPointFromQuickCompiledCode(GetQuickResolutionTrampoline(runtime->GetClassLinker()));
    method->SetEntryPointFromPortableCompiledCode(GetPortableResolutionTrampoline(runtime->GetClassLinker()));
  } else if (enter_interpreter) {
    if (!method->IsNative()) {
      // Set entry point from compiled code if there's no code or in interpreter only mode.
      method->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
      method->SetEntryPointFromPortableCompiledCode(GetPortableToInterpreterBridge());
    } else {
      method->SetEntryPointFromQuickCompiledCode(GetQuickGenericJniTrampoline());
      method->SetEntryPointFromPortableCompiledCode(GetPortableToQuickBridge());
    }
  } else if (method->GetEntryPointFromPortableCompiledCode() != nullptr) {
    DCHECK(method->GetEntryPointFromQuickCompiledCode() == nullptr);
    have_portable_code = true;
    method->SetEntryPointFromQuickCompiledCode(GetQuickToPortableBridge());
  } else {
    DCHECK(method->GetEntryPointFromQuickCompiledCode() != nullptr);
    method->SetEntryPointFromPortableCompiledCode(GetPortableToQuickBridge());
  }

  if (method->IsNative()) {
    // Unregistering restores the dlsym lookup stub.
    method->UnregisterNative(Thread::Current());

    if (enter_interpreter) {
      // We have a native method here without code. Then it should have either the GenericJni
      // trampoline as entrypoint (non-static), or the Resolution trampoline (static).
      DCHECK(method->GetEntryPointFromQuickCompiledCode() ==
          GetQuickResolutionTrampoline(runtime->GetClassLinker())
          || method->GetEntryPointFromQuickCompiledCode() == GetQuickGenericJniTrampoline());

      DCHECK_EQ(method->GetFrameSizeInBytes<false>(), 0U);

      // Fix up method metadata if necessary.
      uint32_t s_len;
      const char* shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(dex_method_index), &s_len);
      uint32_t refs = 1;    // Native method always has "this" or class.
      for (uint32_t i = 1; i < s_len; ++i) {
        if (shorty[i] == 'L') {
          refs++;
        }
      }
      size_t sirt_size = StackIndirectReferenceTable::GetAlignedSirtSize(refs);

      // Get the generic spill masks and base frame size.
      mirror::ArtMethod* callee_save_method =
          Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs);

      method->SetFrameSizeInBytes(callee_save_method->GetFrameSizeInBytes() + sirt_size);
      method->SetCoreSpillMask(callee_save_method->GetCoreSpillMask());
      method->SetFpSpillMask(callee_save_method->GetFpSpillMask());
    }
  }

  // Allow instrumentation its chance to hijack code.
  runtime->GetInstrumentation()->UpdateMethodsCode(method.get(),
                                                   method->GetEntryPointFromQuickCompiledCode(),
                                                   method->GetEntryPointFromPortableCompiledCode(),
                                                   have_portable_code);
}

void ClassLinker::LoadClass(const DexFile& dex_file,
                            const DexFile::ClassDef& dex_class_def,
                            const SirtRef<mirror::Class>& klass,
                            mirror::ClassLoader* class_loader) {
  CHECK(klass.get() != NULL);
  CHECK(klass->GetDexCache() != NULL);
  CHECK_EQ(mirror::Class::kStatusNotReady, klass->GetStatus());
  const char* descriptor = dex_file.GetClassDescriptor(dex_class_def);
  CHECK(descriptor != NULL);

  klass->SetClass(GetClassRoot(kJavaLangClass));
  if (kUseBakerOrBrooksReadBarrier) {
    klass->AssertReadBarrierPointer();
  }
  uint32_t access_flags = dex_class_def.access_flags_;
  // Make sure that none of our runtime-only flags are set.
  CHECK_EQ(access_flags & ~kAccJavaFlagsMask, 0U);
  klass->SetAccessFlags(access_flags);
  klass->SetClassLoader(class_loader);
  DCHECK_EQ(klass->GetPrimitiveType(), Primitive::kPrimNot);
  klass->SetStatus(mirror::Class::kStatusIdx, NULL);

  klass->SetDexClassDefIndex(dex_file.GetIndexForClassDef(dex_class_def));
  klass->SetDexTypeIndex(dex_class_def.class_idx_);

  const byte* class_data = dex_file.GetClassData(dex_class_def);
  if (class_data == NULL) {
    return;  // no fields or methods - for example a marker interface
  }

  if (Runtime::Current()->IsStarted() && !Runtime::Current()->UseCompileTimeClassPath()) {
    const OatFile::OatClass oat_class = GetOatClass(dex_file, klass->GetDexClassDefIndex());
    LoadClassMembers(dex_file, class_data, klass, class_loader, &oat_class);
  } else {
    LoadClassMembers(dex_file, class_data, klass, class_loader, nullptr);
  }
}

void ClassLinker::LoadClassMembers(const DexFile& dex_file,
                                   const byte* class_data,
                                   const SirtRef<mirror::Class>& klass,
                                   mirror::ClassLoader* class_loader,
                                   const OatFile::OatClass* oat_class) {
  // Load fields.
  ClassDataItemIterator it(dex_file, class_data);
  Thread* self = Thread::Current();
  if (it.NumStaticFields() != 0) {
    mirror::ObjectArray<mirror::ArtField>* statics = AllocArtFieldArray(self, it.NumStaticFields());
    if (UNLIKELY(statics == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetSFields(statics);
  }
  if (it.NumInstanceFields() != 0) {
    mirror::ObjectArray<mirror::ArtField>* fields =
        AllocArtFieldArray(self, it.NumInstanceFields());
    if (UNLIKELY(fields == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetIFields(fields);
  }
  for (size_t i = 0; it.HasNextStaticField(); i++, it.Next()) {
    SirtRef<mirror::ArtField> sfield(self, AllocArtField(self));
    if (UNLIKELY(sfield.get() == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetStaticField(i, sfield.get());
    LoadField(dex_file, it, klass, sfield);
  }
  for (size_t i = 0; it.HasNextInstanceField(); i++, it.Next()) {
    SirtRef<mirror::ArtField> ifield(self, AllocArtField(self));
    if (UNLIKELY(ifield.get() == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetInstanceField(i, ifield.get());
    LoadField(dex_file, it, klass, ifield);
  }

  // Load methods.
  if (it.NumDirectMethods() != 0) {
    // TODO: append direct methods to class object
    mirror::ObjectArray<mirror::ArtMethod>* directs =
         AllocArtMethodArray(self, it.NumDirectMethods());
    if (UNLIKELY(directs == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetDirectMethods(directs);
  }
  if (it.NumVirtualMethods() != 0) {
    // TODO: append direct methods to class object
    mirror::ObjectArray<mirror::ArtMethod>* virtuals =
        AllocArtMethodArray(self, it.NumVirtualMethods());
    if (UNLIKELY(virtuals == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetVirtualMethods(virtuals);
  }
  size_t class_def_method_index = 0;
  for (size_t i = 0; it.HasNextDirectMethod(); i++, it.Next()) {
    SirtRef<mirror::ArtMethod> method(self, LoadMethod(self, dex_file, it, klass));
    if (UNLIKELY(method.get() == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetDirectMethod(i, method.get());
    if (oat_class != nullptr) {
      LinkCode(method, oat_class, dex_file, it.GetMemberIndex(), class_def_method_index);
    }
    method->SetMethodIndex(class_def_method_index);
    class_def_method_index++;
  }
  for (size_t i = 0; it.HasNextVirtualMethod(); i++, it.Next()) {
    SirtRef<mirror::ArtMethod> method(self, LoadMethod(self, dex_file, it, klass));
    if (UNLIKELY(method.get() == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return;
    }
    klass->SetVirtualMethod(i, method.get());
    DCHECK_EQ(class_def_method_index, it.NumDirectMethods() + i);
    if (oat_class != nullptr) {
      LinkCode(method, oat_class, dex_file, it.GetMemberIndex(), class_def_method_index);
    }
    class_def_method_index++;
  }
  DCHECK(!it.HasNext());
}

void ClassLinker::LoadField(const DexFile& /*dex_file*/, const ClassDataItemIterator& it,
                            const SirtRef<mirror::Class>& klass,
                            const SirtRef<mirror::ArtField>& dst) {
  uint32_t field_idx = it.GetMemberIndex();
  dst->SetDexFieldIndex(field_idx);
  dst->SetDeclaringClass(klass.get());
  dst->SetAccessFlags(it.GetMemberAccessFlags());
}

mirror::ArtMethod* ClassLinker::LoadMethod(Thread* self, const DexFile& dex_file,
                                           const ClassDataItemIterator& it,
                                           const SirtRef<mirror::Class>& klass) {
  uint32_t dex_method_idx = it.GetMemberIndex();
  const DexFile::MethodId& method_id = dex_file.GetMethodId(dex_method_idx);
  const char* method_name = dex_file.StringDataByIdx(method_id.name_idx_);

  mirror::ArtMethod* dst = AllocArtMethod(self);
  if (UNLIKELY(dst == NULL)) {
    CHECK(self->IsExceptionPending());  // OOME.
    return NULL;
  }
  DCHECK(dst->IsArtMethod()) << PrettyDescriptor(dst->GetClass());

  const char* old_cause = self->StartAssertNoThreadSuspension("LoadMethod");
  dst->SetDexMethodIndex(dex_method_idx);
  dst->SetDeclaringClass(klass.get());
  dst->SetCodeItemOffset(it.GetMethodCodeItemOffset());

  dst->SetDexCacheStrings(klass->GetDexCache()->GetStrings());
  dst->SetDexCacheResolvedMethods(klass->GetDexCache()->GetResolvedMethods());
  dst->SetDexCacheResolvedTypes(klass->GetDexCache()->GetResolvedTypes());

  uint32_t access_flags = it.GetMemberAccessFlags();

  if (UNLIKELY(strcmp("finalize", method_name) == 0)) {
    // Set finalizable flag on declaring class.
    if (strcmp("V", dex_file.GetShorty(method_id.proto_idx_)) == 0) {
      // Void return type.
      if (klass->GetClassLoader() != NULL) {  // All non-boot finalizer methods are flagged
        klass->SetFinalizable();
      } else {
        ClassHelper kh(klass.get());
        const char* klass_descriptor = kh.GetDescriptor();
        // The Enum class declares a "final" finalize() method to prevent subclasses from
        // introducing a finalizer. We don't want to set the finalizable flag for Enum or its
        // subclasses, so we exclude it here.
        // We also want to avoid setting the flag on Object, where we know that finalize() is
        // empty.
        if ((strcmp("Ljava/lang/Object;", klass_descriptor) != 0) &&
            (strcmp("Ljava/lang/Enum;", klass_descriptor) != 0)) {
          klass->SetFinalizable();
        }
      }
    }
  } else if (method_name[0] == '<') {
    // Fix broken access flags for initializers. Bug 11157540.
    bool is_init = (strcmp("<init>", method_name) == 0);
    bool is_clinit = !is_init && (strcmp("<clinit>", method_name) == 0);
    if (UNLIKELY(!is_init && !is_clinit)) {
      LOG(WARNING) << "Unexpected '<' at start of method name " << method_name;
    } else {
      if (UNLIKELY((access_flags & kAccConstructor) == 0)) {
        LOG(WARNING) << method_name << " didn't have expected constructor access flag in class "
            << PrettyDescriptor(klass.get()) << " in dex file " << dex_file.GetLocation();
        access_flags |= kAccConstructor;
      }
    }
  }
  dst->SetAccessFlags(access_flags);

  self->EndAssertNoThreadSuspension(old_cause);
  return dst;
}

void ClassLinker::AppendToBootClassPath(const DexFile& dex_file) {
  Thread* self = Thread::Current();
  SirtRef<mirror::DexCache> dex_cache(self, AllocDexCache(self, dex_file));
  CHECK(dex_cache.get() != NULL) << "Failed to allocate dex cache for " << dex_file.GetLocation();
  AppendToBootClassPath(dex_file, dex_cache);
}

void ClassLinker::AppendToBootClassPath(const DexFile& dex_file,
                                        const SirtRef<mirror::DexCache>& dex_cache) {
  CHECK(dex_cache.get() != NULL) << dex_file.GetLocation();
  boot_class_path_.push_back(&dex_file);
  RegisterDexFile(dex_file, dex_cache);
}

bool ClassLinker::IsDexFileRegisteredLocked(const DexFile& dex_file) const {
  dex_lock_.AssertSharedHeld(Thread::Current());
  std::map<const DexFile*, mirror::DexCache*>::const_iterator it = dex_caches_.find(&dex_file);
  if (it != dex_caches_.end()){
      return true;
  }
//  for (size_t i = 0; i != dex_caches_.size(); ++i) {
//    if (dex_caches_[i]->GetDexFile() == &dex_file) {
//      return true;
//    }
//  }
  return false;
}

bool ClassLinker::IsDexFileRegistered(const DexFile& dex_file) const {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  return IsDexFileRegisteredLocked(dex_file);
}

void ClassLinker::RegisterDexFileLocked(const DexFile& dex_file,
                                        const SirtRef<mirror::DexCache>& dex_cache) {
  dex_lock_.AssertExclusiveHeld(Thread::Current());
  CHECK(dex_cache.get() != NULL) << dex_file.GetLocation();
  CHECK(dex_cache->GetLocation()->Equals(dex_file.GetLocation()))
      << dex_cache->GetLocation()->ToModifiedUtf8() << " " << dex_file.GetLocation();
  dex_caches_.insert(std::make_pair(&dex_file, dex_cache.get()));
  dex_cache->SetDexFile(&dex_file);
  if (log_new_dex_caches_roots_) {
    // TODO: This is not safe if we can remove dex caches.
    new_dex_cache_roots_.push_back(&dex_file);
  }
}

void ClassLinker::RegisterDexFile(const DexFile& dex_file) {
  Thread* self = Thread::Current();
  {
    ReaderMutexLock mu(self, dex_lock_);
    if (IsDexFileRegisteredLocked(dex_file)) {
      return;
    }
  }
  // Don't alloc while holding the lock, since allocation may need to
  // suspend all threads and another thread may need the dex_lock_ to
  // get to a suspend point.
  SirtRef<mirror::DexCache> dex_cache(self, AllocDexCache(self, dex_file));
  CHECK(dex_cache.get() != NULL) << "Failed to allocate dex cache for " << dex_file.GetLocation();
  {
    WriterMutexLock mu(self, dex_lock_);
    if (IsDexFileRegisteredLocked(dex_file)) {
      return;
    }
    RegisterDexFileLocked(dex_file, dex_cache);
  }
}

void ClassLinker::RegisterDexFile(const DexFile& dex_file,
                                  const SirtRef<mirror::DexCache>& dex_cache) {
  WriterMutexLock mu(Thread::Current(), dex_lock_);
  RegisterDexFileLocked(dex_file, dex_cache);
}

mirror::DexCache* ClassLinker::FindDexCache(const DexFile& dex_file) const {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  // Search assuming unique-ness of dex file.
  std::map<const DexFile*, mirror::DexCache*>::const_iterator it = dex_caches_.find(&dex_file);
  if (it != dex_caches_.end()) {
      return it->second;
  }
//  for (size_t i = 0; i != dex_caches_.size(); ++i) {
//    mirror::DexCache* dex_cache = dex_caches_[i];
//    if (dex_cache->GetDexFile() == &dex_file) {
//      return dex_cache;
//    }
//  }

  // Search matching by location name.
  std::string location(dex_file.GetLocation());
  for (auto dex_cache : dex_caches_) {
      if (dex_cache.second->GetDexFile()->GetLocation() == location){
          return dex_cache.second;
      }
  }
//  for (size_t i = 0; i != dex_caches_.size(); ++i) {
//    mirror::DexCache* dex_cache = dex_caches_[i];
//    if (dex_cache->GetDexFile()->GetLocation() == location) {
//      return dex_cache;
//    }
//  }

  // Failure, dump diagnostic and abort.
  for (auto dex_cache : dex_caches_) {
      if (dex_cache.second->GetDexFile()->GetLocation() == location){
          LOG(ERROR) << "Registered dex file " << dex_cache.first << " = " << dex_cache.second->GetDexFile()->GetLocation();
      }
  }
//  for (size_t i = 0; i != dex_caches_.size(); ++i) {
//    mirror::DexCache* dex_cache = dex_caches_[i];
//    LOG(ERROR) << "Registered dex file " << i << " = " << dex_cache->GetDexFile()->GetLocation();
//  }
  LOG(FATAL) << "Failed to find DexCache for DexFile " << location;
  return NULL;
}

void ClassLinker::FixupDexCaches(mirror::ArtMethod* resolution_method) const {
  ReaderMutexLock mu(Thread::Current(), dex_lock_);
  for (auto dex_cache : dex_caches_) {
      dex_cache.second->Fixup(resolution_method);
  }
}

mirror::Class* ClassLinker::CreatePrimitiveClass(Thread* self, Primitive::Type type) {
  mirror::Class* klass = AllocClass(self, sizeof(mirror::Class));
  if (UNLIKELY(klass == NULL)) {
    return NULL;
  }
  return InitializePrimitiveClass(klass, type);
}

mirror::Class* ClassLinker::InitializePrimitiveClass(mirror::Class* primitive_class,
                                                     Primitive::Type type) {
  CHECK(primitive_class != NULL);
  // Must hold lock on object when initializing.
  Thread* self = Thread::Current();
  SirtRef<mirror::Class> sirt_class(self, primitive_class);
  ObjectLock<mirror::Class> lock(self, &sirt_class);
  primitive_class->SetAccessFlags(kAccPublic | kAccFinal | kAccAbstract);
  primitive_class->SetPrimitiveType(type);
  primitive_class->SetStatus(mirror::Class::kStatusInitialized, self);
  const char* descriptor = Primitive::Descriptor(type);
  mirror::Class* existing = InsertClass(descriptor, primitive_class, Hash(descriptor));
  CHECK(existing == NULL) << "InitPrimitiveClass(" << type << ") failed";
  return primitive_class;
}

// Create an array class (i.e. the class object for the array, not the
// array itself).  "descriptor" looks like "[C" or "[[[[B" or
// "[Ljava/lang/String;".
//
// If "descriptor" refers to an array of primitives, look up the
// primitive type's internally-generated class object.
//
// "class_loader" is the class loader of the class that's referring to
// us.  It's used to ensure that we're looking for the element type in
// the right context.  It does NOT become the class loader for the
// array class; that always comes from the base element class.
//
// Returns NULL with an exception raised on failure.
mirror::Class* ClassLinker::CreateArrayClass(Thread* self, const char* descriptor,
                                             const SirtRef<mirror::ClassLoader>& class_loader) {
  // Identify the underlying component type
  CHECK_EQ('[', descriptor[0]);
  SirtRef<mirror::Class> component_type(self, FindClass(self, descriptor + 1, class_loader));
  if (component_type.get() == nullptr) {
    DCHECK(self->IsExceptionPending());
    return nullptr;
  }

  // See if the component type is already loaded.  Array classes are
  // always associated with the class loader of their underlying
  // element type -- an array of Strings goes with the loader for
  // java/lang/String -- so we need to look for it there.  (The
  // caller should have checked for the existence of the class
  // before calling here, but they did so with *their* class loader,
  // not the component type's loader.)
  //
  // If we find it, the caller adds "loader" to the class' initiating
  // loader list, which should prevent us from going through this again.
  //
  // This call is unnecessary if "loader" and "component_type->GetClassLoader()"
  // are the same, because our caller (FindClass) just did the
  // lookup.  (Even if we get this wrong we still have correct behavior,
  // because we effectively do this lookup again when we add the new
  // class to the hash table --- necessary because of possible races with
  // other threads.)
  if (class_loader.get() != component_type->GetClassLoader()) {
    mirror::Class* new_class = LookupClass(descriptor, component_type->GetClassLoader());
    if (new_class != NULL) {
      return new_class;
    }
  }

  // Fill out the fields in the Class.
  //
  // It is possible to execute some methods against arrays, because
  // all arrays are subclasses of java_lang_Object_, so we need to set
  // up a vtable.  We can just point at the one in java_lang_Object_.
  //
  // Array classes are simple enough that we don't need to do a full
  // link step.
  SirtRef<mirror::Class> new_class(self, NULL);
  if (UNLIKELY(!init_done_)) {
    // Classes that were hand created, ie not by FindSystemClass
    if (strcmp(descriptor, "[Ljava/lang/Class;") == 0) {
      new_class.reset(GetClassRoot(kClassArrayClass));
    } else if (strcmp(descriptor, "[Ljava/lang/Object;") == 0) {
      new_class.reset(GetClassRoot(kObjectArrayClass));
    } else if (strcmp(descriptor, class_roots_descriptors_[kJavaLangStringArrayClass]) == 0) {
      new_class.reset(GetClassRoot(kJavaLangStringArrayClass));
    } else if (strcmp(descriptor,
                      class_roots_descriptors_[kJavaLangReflectArtMethodArrayClass]) == 0) {
      new_class.reset(GetClassRoot(kJavaLangReflectArtMethodArrayClass));
    } else if (strcmp(descriptor,
                      class_roots_descriptors_[kJavaLangReflectArtFieldArrayClass]) == 0) {
      new_class.reset(GetClassRoot(kJavaLangReflectArtFieldArrayClass));
    } else if (strcmp(descriptor, "[C") == 0) {
      new_class.reset(GetClassRoot(kCharArrayClass));
    } else if (strcmp(descriptor, "[I") == 0) {
      new_class.reset(GetClassRoot(kIntArrayClass));
    }
  }
  if (new_class.get() == nullptr) {
    new_class.reset(AllocClass(self, sizeof(mirror::Class)));
    if (new_class.get() == nullptr) {
      return nullptr;
    }
    new_class->SetComponentType(component_type.get());
  }
  ObjectLock<mirror::Class> lock(self, &new_class);  // Must hold lock on object when initializing.
  DCHECK(new_class->GetComponentType() != NULL);
  mirror::Class* java_lang_Object = GetClassRoot(kJavaLangObject);
  new_class->SetSuperClass(java_lang_Object);
  new_class->SetVTable(java_lang_Object->GetVTable());
  new_class->SetPrimitiveType(Primitive::kPrimNot);
  new_class->SetClassLoader(component_type->GetClassLoader());
  new_class->SetStatus(mirror::Class::kStatusInitialized, self);
  // don't need to set new_class->SetObjectSize(..)
  // because Object::SizeOf delegates to Array::SizeOf


  // All arrays have java/lang/Cloneable and java/io/Serializable as
  // interfaces.  We need to set that up here, so that stuff like
  // "instanceof" works right.
  //
  // Note: The GC could run during the call to FindSystemClass,
  // so we need to make sure the class object is GC-valid while we're in
  // there.  Do this by clearing the interface list so the GC will just
  // think that the entries are null.


  // Use the single, global copies of "interfaces" and "iftable"
  // (remember not to free them for arrays).
  CHECK(array_iftable_ != nullptr);
  new_class->SetIfTable(array_iftable_);

  // Inherit access flags from the component type.
  int access_flags = new_class->GetComponentType()->GetAccessFlags();
  // Lose any implementation detail flags; in particular, arrays aren't finalizable.
  access_flags &= kAccJavaFlagsMask;
  // Arrays can't be used as a superclass or interface, so we want to add "abstract final"
  // and remove "interface".
  access_flags |= kAccAbstract | kAccFinal;
  access_flags &= ~kAccInterface;

  new_class->SetAccessFlags(access_flags);

  mirror::Class* existing = InsertClass(descriptor, new_class.get(), Hash(descriptor));
  if (existing == nullptr) {
    return new_class.get();
  }
  // Another thread must have loaded the class after we
  // started but before we finished.  Abandon what we've
  // done.
  //
  // (Yes, this happens.)

  return existing;
}

mirror::Class* ClassLinker::FindPrimitiveClass(char type) {
  switch (type) {
    case 'B':
      return GetClassRoot(kPrimitiveByte);
    case 'C':
      return GetClassRoot(kPrimitiveChar);
    case 'D':
      return GetClassRoot(kPrimitiveDouble);
    case 'F':
      return GetClassRoot(kPrimitiveFloat);
    case 'I':
      return GetClassRoot(kPrimitiveInt);
    case 'J':
      return GetClassRoot(kPrimitiveLong);
    case 'S':
      return GetClassRoot(kPrimitiveShort);
    case 'Z':
      return GetClassRoot(kPrimitiveBoolean);
    case 'V':
      return GetClassRoot(kPrimitiveVoid);
    default:
      break;
  }
  std::string printable_type(PrintableChar(type));
  ThrowNoClassDefFoundError("Not a primitive type: %s", printable_type.c_str());
  return NULL;
}

mirror::Class* ClassLinker::InsertClass(const char* descriptor, mirror::Class* klass,
                                        size_t hash) {
  if (VLOG_IS_ON(class_linker)) {
    mirror::DexCache* dex_cache = klass->GetDexCache();
    std::string source;
    if (dex_cache != NULL) {
      source += " from ";
      source += dex_cache->GetLocation()->ToModifiedUtf8();
    }
    LOG(INFO) << "Loaded class " << descriptor << source;
  }
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  mirror::Class* existing =
      LookupClassFromTableLocked(descriptor, klass->GetClassLoader(), hash);
  if (existing != NULL) {
    return existing;
  }
  if (kIsDebugBuild && klass->GetClassLoader() == NULL && dex_cache_image_class_lookup_required_) {
    // Check a class loaded with the system class loader matches one in the image if the class
    // is in the image.
    existing = LookupClassFromImage(descriptor);
    if (existing != NULL) {
      CHECK(klass == existing);
    }
  }
  VerifyObject(klass);
  class_table_.insert(std::make_pair(hash, klass));
  if (log_new_class_table_roots_) {
    new_class_roots_.push_back(std::make_pair(hash, klass));
  }
  return NULL;
}

bool ClassLinker::RemoveClass(const char* descriptor, const mirror::ClassLoader* class_loader) {
  size_t hash = Hash(descriptor);
  WriterMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  for (auto it = class_table_.lower_bound(hash), end = class_table_.end(); it != end && it->first == hash;
       ++it) {
    mirror::Class* klass = it->second;
    ClassHelper kh(klass);
    if ((klass->GetClassLoader() == class_loader) &&
        (strcmp(descriptor, kh.GetDescriptor()) == 0)) {
      class_table_.erase(it);
      return true;
    }
  }
  return false;
}

mirror::Class* ClassLinker::LookupClass(const char* descriptor,
                                        const mirror::ClassLoader* class_loader) {
  size_t hash = Hash(descriptor);
  {
    ReaderMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
    mirror::Class* result = LookupClassFromTableLocked(descriptor, class_loader, hash);
    if (result != NULL) {
      return result;
    }
  }
  if (class_loader != NULL || !dex_cache_image_class_lookup_required_) {
    return NULL;
  } else {
    // Lookup failed but need to search dex_caches_.
    mirror::Class* result = LookupClassFromImage(descriptor);
    if (result != NULL) {
      InsertClass(descriptor, result, hash);
    } else {
      // Searching the image dex files/caches failed, we don't want to get into this situation
      // often as map searches are faster, so after kMaxFailedDexCacheLookups move all image
      // classes into the class table.
      const int32_t kMaxFailedDexCacheLookups = 1000;
      if (++failed_dex_cache_class_lookups_ > kMaxFailedDexCacheLookups) {
        MoveImageClassesToClassTable();
      }
    }
    return result;
  }
}

mirror::Class* ClassLinker::LookupClassFromTableLocked(const char* descriptor,
                                                       const mirror::ClassLoader* class_loader,
                                                       size_t hash) {
  auto end = class_table_.end();
  for (auto it = class_table_.lower_bound(hash); it != end && it->first == hash; ++it) {
    mirror::Class* klass = it->second;
    ClassHelper kh(klass);
    if ((klass->GetClassLoader() == class_loader) &&
        (strcmp(descriptor, kh.GetDescriptor()) == 0)) {
      if (kIsDebugBuild) {
        // Check for duplicates in the table.
        for (++it; it != end && it->first == hash; ++it) {
          mirror::Class* klass2 = it->second;
          ClassHelper kh(klass2);
          CHECK(!((klass2->GetClassLoader() == class_loader) &&
                  (strcmp(descriptor, kh.GetDescriptor()) == 0)))
              << PrettyClass(klass) << " " << klass << " " << klass->GetClassLoader() << " "
              << PrettyClass(klass2) << " " << klass2 << " " << klass2->GetClassLoader();
        }
      }
      return klass;
    }
  }
  return NULL;
}

static mirror::ObjectArray<mirror::DexCache>* GetImageDexCaches()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  gc::space::ImageSpace* image = Runtime::Current()->GetHeap()->GetImageSpace();
  CHECK(image != NULL);
  mirror::Object* root = image->GetImageHeader().GetImageRoot(ImageHeader::kDexCaches);
  return root->AsObjectArray<mirror::DexCache>();
}

void ClassLinker::MoveImageClassesToClassTable() {
  Thread* self = Thread::Current();
  WriterMutexLock mu(self, *Locks::classlinker_classes_lock_);
  if (!dex_cache_image_class_lookup_required_) {
    return;  // All dex cache classes are already in the class table.
  }
  const char* old_no_suspend_cause =
      self->StartAssertNoThreadSuspension("Moving image classes to class table");
  mirror::ObjectArray<mirror::DexCache>* dex_caches = GetImageDexCaches();
  for (int32_t i = 0; i < dex_caches->GetLength(); i++) {
    mirror::DexCache* dex_cache = dex_caches->Get(i);
    mirror::ObjectArray<mirror::Class>* types = dex_cache->GetResolvedTypes();
    for (int32_t j = 0; j < types->GetLength(); j++) {
      mirror::Class* klass = types->Get(j);
      if (klass != NULL) {
        ClassHelper kh(klass);
        DCHECK(klass->GetClassLoader() == NULL);
        const char* descriptor = kh.GetDescriptor();
        size_t hash = Hash(descriptor);
        mirror::Class* existing = LookupClassFromTableLocked(descriptor, NULL, hash);
        if (existing != NULL) {
          CHECK(existing == klass) << PrettyClassAndClassLoader(existing) << " != "
              << PrettyClassAndClassLoader(klass);
        } else {
          class_table_.insert(std::make_pair(hash, klass));
          if (log_new_class_table_roots_) {
            new_class_roots_.push_back(std::make_pair(hash, klass));
          }
        }
      }
    }
  }
  dex_cache_image_class_lookup_required_ = false;
  self->EndAssertNoThreadSuspension(old_no_suspend_cause);
}

mirror::Class* ClassLinker::LookupClassFromImage(const char* descriptor) {
  Thread* self = Thread::Current();
  const char* old_no_suspend_cause =
      self->StartAssertNoThreadSuspension("Image class lookup");
  mirror::ObjectArray<mirror::DexCache>* dex_caches = GetImageDexCaches();
  for (int32_t i = 0; i < dex_caches->GetLength(); ++i) {
    mirror::DexCache* dex_cache = dex_caches->Get(i);
    const DexFile* dex_file = dex_cache->GetDexFile();
    // Try binary searching the string/type index.
    const DexFile::StringId* string_id = dex_file->FindStringId(descriptor);
    if (string_id != NULL) {
      const DexFile::TypeId* type_id =
          dex_file->FindTypeId(dex_file->GetIndexForStringId(*string_id));
      if (type_id != NULL) {
        uint16_t type_idx = dex_file->GetIndexForTypeId(*type_id);
        mirror::Class* klass = dex_cache->GetResolvedType(type_idx);
        if (klass != NULL) {
          self->EndAssertNoThreadSuspension(old_no_suspend_cause);
          return klass;
        }
      }
    }
  }
  self->EndAssertNoThreadSuspension(old_no_suspend_cause);
  return NULL;
}

void ClassLinker::LookupClasses(const char* descriptor, std::vector<mirror::Class*>& result) {
  result.clear();
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  size_t hash = Hash(descriptor);
  ReaderMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  for (auto it = class_table_.lower_bound(hash), end = class_table_.end();
      it != end && it->first == hash; ++it) {
    mirror::Class* klass = it->second;
    ClassHelper kh(klass);
    if (strcmp(descriptor, kh.GetDescriptor()) == 0) {
      result.push_back(klass);
    }
  }
}

void ClassLinker::VerifyClass(const SirtRef<mirror::Class>& klass) {
  // TODO: assert that the monitor on the Class is held
  Thread* self = Thread::Current();
  ObjectLock<mirror::Class> lock(self, &klass);

  // Don't attempt to re-verify if already sufficiently verified.
  if (klass->IsVerified() ||
      (klass->IsCompileTimeVerified() && Runtime::Current()->IsCompiler())) {
    return;
  }

  // The class might already be erroneous, for example at compile time if we attempted to verify
  // this class as a parent to another.
  if (klass->IsErroneous()) {
    ThrowEarlierClassFailure(klass.get());
    return;
  }

  if (klass->GetStatus() == mirror::Class::kStatusResolved) {
    klass->SetStatus(mirror::Class::kStatusVerifying, self);
  } else {
    CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusRetryVerificationAtRuntime)
        << PrettyClass(klass.get());
    CHECK(!Runtime::Current()->IsCompiler());
    klass->SetStatus(mirror::Class::kStatusVerifyingAtRuntime, self);
  }

  // Skip verification if disabled.
  if (!Runtime::Current()->IsVerificationEnabled()) {
    klass->SetStatus(mirror::Class::kStatusVerified, self);
    return;
  }

  // Verify super class.
  SirtRef<mirror::Class> super(self, klass->GetSuperClass());
  if (super.get() != NULL) {
    // Acquire lock to prevent races on verifying the super class.
    ObjectLock<mirror::Class> lock(self, &super);

    if (!super->IsVerified() && !super->IsErroneous()) {
      VerifyClass(super);
    }
    if (!super->IsCompileTimeVerified()) {
      std::string error_msg(StringPrintf("Rejecting class %s that attempts to sub-class erroneous class %s",
                                         PrettyDescriptor(klass.get()).c_str(),
                                         PrettyDescriptor(super.get()).c_str()));
      LOG(ERROR) << error_msg  << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8();
      SirtRef<mirror::Throwable> cause(self, self->GetException(NULL));
      if (cause.get() != nullptr) {
        self->ClearException();
      }
      ThrowVerifyError(klass.get(), "%s", error_msg.c_str());
      if (cause.get() != nullptr) {
        self->GetException(nullptr)->SetCause(cause.get());
      }
      ClassReference ref(klass->GetDexCache()->GetDexFile(), klass->GetDexClassDefIndex());
      if (Runtime::Current()->IsCompiler()) {
        Runtime::Current()->GetCompilerCallbacks()->ClassRejected(ref);
      }
      klass->SetStatus(mirror::Class::kStatusError, self);
      return;
    }
  }

  // Try to use verification information from the oat file, otherwise do runtime verification.
  const DexFile& dex_file = *klass->GetDexCache()->GetDexFile();
  mirror::Class::Status oat_file_class_status(mirror::Class::kStatusNotReady);
  bool preverified = VerifyClassUsingOatFile(dex_file, klass.get(), oat_file_class_status);
  if (oat_file_class_status == mirror::Class::kStatusError) {
    VLOG(class_linker) << "Skipping runtime verification of erroneous class "
        << PrettyDescriptor(klass.get()) << " in "
        << klass->GetDexCache()->GetLocation()->ToModifiedUtf8();
    ThrowVerifyError(klass.get(), "Rejecting class %s because it failed compile-time verification",
                     PrettyDescriptor(klass.get()).c_str());
    klass->SetStatus(mirror::Class::kStatusError, self);
    return;
  }
  verifier::MethodVerifier::FailureKind verifier_failure = verifier::MethodVerifier::kNoFailure;
  std::string error_msg;
  if (!preverified) {
    verifier_failure = verifier::MethodVerifier::VerifyClass(klass.get(),
                                                             Runtime::Current()->IsCompiler(),
                                                             &error_msg);
  }
  if (preverified || verifier_failure != verifier::MethodVerifier::kHardFailure) {
    if (!preverified && verifier_failure != verifier::MethodVerifier::kNoFailure) {
      VLOG(class_linker) << "Soft verification failure in class " << PrettyDescriptor(klass.get())
          << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8()
          << " because: " << error_msg;
    }
    self->AssertNoPendingException();
    // Make sure all classes referenced by catch blocks are resolved.
    ResolveClassExceptionHandlerTypes(dex_file, klass);
    if (verifier_failure == verifier::MethodVerifier::kNoFailure) {
      // Even though there were no verifier failures we need to respect whether the super-class
      // was verified or requiring runtime reverification.
      if (super.get() == NULL || super->IsVerified()) {
        klass->SetStatus(mirror::Class::kStatusVerified, self);
      } else {
        CHECK_EQ(super->GetStatus(), mirror::Class::kStatusRetryVerificationAtRuntime);
        klass->SetStatus(mirror::Class::kStatusRetryVerificationAtRuntime, self);
        // Pretend a soft failure occured so that we don't consider the class verified below.
        verifier_failure = verifier::MethodVerifier::kSoftFailure;
      }
    } else {
      CHECK_EQ(verifier_failure, verifier::MethodVerifier::kSoftFailure);
      // Soft failures at compile time should be retried at runtime. Soft
      // failures at runtime will be handled by slow paths in the generated
      // code. Set status accordingly.
      if (Runtime::Current()->IsCompiler()) {
        klass->SetStatus(mirror::Class::kStatusRetryVerificationAtRuntime, self);
      } else {
        klass->SetStatus(mirror::Class::kStatusVerified, self);
      }
    }
  } else {
    LOG(ERROR) << "Verification failed on class " << PrettyDescriptor(klass.get())
        << " in " << klass->GetDexCache()->GetLocation()->ToModifiedUtf8()
        << " because: " << error_msg;
    self->AssertNoPendingException();
    ThrowVerifyError(klass.get(), "%s", error_msg.c_str());
    klass->SetStatus(mirror::Class::kStatusError, self);
  }
  if (preverified || verifier_failure == verifier::MethodVerifier::kNoFailure) {
    // Class is verified so we don't need to do any access check on its methods.
    // Let the interpreter know it by setting the kAccPreverified flag onto each
    // method.
    // Note: we're going here during compilation and at runtime. When we set the
    // kAccPreverified flag when compiling image classes, the flag is recorded
    // in the image and is set when loading the image.
    klass->SetPreverifiedFlagOnAllMethods();
  }
}

bool ClassLinker::VerifyClassUsingOatFile(const DexFile& dex_file, mirror::Class* klass,
                                          mirror::Class::Status& oat_file_class_status) {
  // If we're compiling, we can only verify the class using the oat file if
  // we are not compiling the image or if the class we're verifying is not part of
  // the app.  In other words, we will only check for preverification of bootclasspath
  // classes.
  if (Runtime::Current()->IsCompiler()) {
    // Are we compiling the bootclasspath?
    if (!Runtime::Current()->UseCompileTimeClassPath()) {
      return false;
    }
    // We are compiling an app (not the image).

    // Is this an app class? (I.e. not a bootclasspath class)
    if (klass->GetClassLoader() != NULL) {
      return false;
    }
  }

  const OatFile* oat_file = FindOpenedOatFileForDexFile(dex_file);
  // Make this work with gtests, which do not set up the image properly.
  // TODO: we should clean up gtests to set up the image path properly.
  if (Runtime::Current()->IsCompiler() && (oat_file == NULL)) {
    return false;
  }

  CHECK(oat_file != NULL) << dex_file.GetLocation() << " " << PrettyClass(klass);
  uint dex_location_checksum = dex_file.GetLocationChecksum();
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file.GetLocation().c_str(),
                                                                    &dex_location_checksum);
  CHECK(oat_dex_file != NULL) << dex_file.GetLocation() << " " << PrettyClass(klass);
  uint16_t class_def_index = klass->GetDexClassDefIndex();
  oat_file_class_status = oat_dex_file->GetOatClass(class_def_index).GetStatus();
  if (oat_file_class_status == mirror::Class::kStatusVerified ||
      oat_file_class_status == mirror::Class::kStatusInitialized) {
      return true;
  }
  if (oat_file_class_status == mirror::Class::kStatusRetryVerificationAtRuntime) {
    // Compile time verification failed with a soft error. Compile time verification can fail
    // because we have incomplete type information. Consider the following:
    // class ... {
    //   Foo x;
    //   .... () {
    //     if (...) {
    //       v1 gets assigned a type of resolved class Foo
    //     } else {
    //       v1 gets assigned a type of unresolved class Bar
    //     }
    //     iput x = v1
    // } }
    // when we merge v1 following the if-the-else it results in Conflict
    // (see verifier::RegType::Merge) as we can't know the type of Bar and we could possibly be
    // allowing an unsafe assignment to the field x in the iput (javac may have compiled this as
    // it knew Bar was a sub-class of Foo, but for us this may have been moved into a separate apk
    // at compile time).
    return false;
  }
  if (oat_file_class_status == mirror::Class::kStatusError) {
    // Compile time verification failed with a hard error. This is caused by invalid instructions
    // in the class. These errors are unrecoverable.
    return false;
  }
  if (oat_file_class_status == mirror::Class::kStatusNotReady) {
    // Status is uninitialized if we couldn't determine the status at compile time, for example,
    // not loading the class.
    // TODO: when the verifier doesn't rely on Class-es failing to resolve/load the type hierarchy
    // isn't a problem and this case shouldn't occur
    return false;
  }
  LOG(FATAL) << "Unexpected class status: " << oat_file_class_status
             << " " << dex_file.GetLocation() << " " << PrettyClass(klass) << " "
             << ClassHelper(klass).GetDescriptor();

  return false;
}

void ClassLinker::ResolveClassExceptionHandlerTypes(const DexFile& dex_file,
                                                    const SirtRef<mirror::Class>& klass) {
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    ResolveMethodExceptionHandlerTypes(dex_file, klass->GetDirectMethod(i));
  }
  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    ResolveMethodExceptionHandlerTypes(dex_file, klass->GetVirtualMethod(i));
  }
}

void ClassLinker::ResolveMethodExceptionHandlerTypes(const DexFile& dex_file,
                                                     mirror::ArtMethod* method) {
  // similar to DexVerifier::ScanTryCatchBlocks and dex2oat's ResolveExceptionsForMethod.
  const DexFile::CodeItem* code_item = dex_file.GetCodeItem(method->GetCodeItemOffset());
  if (code_item == NULL) {
    return;  // native or abstract method
  }
  if (code_item->tries_size_ == 0) {
    return;  // nothing to process
  }
  const byte* handlers_ptr = DexFile::GetCatchHandlerData(*code_item, 0);
  uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  for (uint32_t idx = 0; idx < handlers_size; idx++) {
    CatchHandlerIterator iterator(handlers_ptr);
    for (; iterator.HasNext(); iterator.Next()) {
      // Ensure exception types are resolved so that they don't need resolution to be delivered,
      // unresolved exception types will be ignored by exception delivery
      if (iterator.GetHandlerTypeIndex() != DexFile::kDexNoIndex16) {
        mirror::Class* exception_type = linker->ResolveType(iterator.GetHandlerTypeIndex(), method);
        if (exception_type == NULL) {
          DCHECK(Thread::Current()->IsExceptionPending());
          Thread::Current()->ClearException();
        }
      }
    }
    handlers_ptr = iterator.EndDataPointer();
  }
}

static void CheckProxyConstructor(mirror::ArtMethod* constructor);
static void CheckProxyMethod(mirror::ArtMethod* method,
                             SirtRef<mirror::ArtMethod>& prototype);

mirror::Class* ClassLinker::CreateProxyClass(ScopedObjectAccess& soa, jstring name,
                                             jobjectArray interfaces, jobject loader,
                                             jobjectArray methods, jobjectArray throws) {
  Thread* self = soa.Self();
  SirtRef<mirror::Class> klass(self, AllocClass(self, GetClassRoot(kJavaLangClass),
                                                sizeof(mirror::SynthesizedProxyClass)));
  if (klass.get() == NULL) {
    CHECK(self->IsExceptionPending());  // OOME.
    return NULL;
  }
  DCHECK(klass->GetClass() != NULL);
  klass->SetObjectSize(sizeof(mirror::Proxy));
  klass->SetAccessFlags(kAccClassIsProxy | kAccPublic | kAccFinal);
  klass->SetClassLoader(soa.Decode<mirror::ClassLoader*>(loader));
  DCHECK_EQ(klass->GetPrimitiveType(), Primitive::kPrimNot);
  klass->SetName(soa.Decode<mirror::String*>(name));
  mirror::Class* proxy_class = GetClassRoot(kJavaLangReflectProxy);
  klass->SetDexCache(proxy_class->GetDexCache());
  klass->SetStatus(mirror::Class::kStatusIdx, self);

  // Instance fields are inherited, but we add a couple of static fields...
  {
    mirror::ObjectArray<mirror::ArtField>* sfields = AllocArtFieldArray(self, 2);
    if (UNLIKELY(sfields == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return NULL;
    }
    klass->SetSFields(sfields);
  }
  // 1. Create a static field 'interfaces' that holds the _declared_ interfaces implemented by
  // our proxy, so Class.getInterfaces doesn't return the flattened set.
  SirtRef<mirror::ArtField> interfaces_sfield(self, AllocArtField(self));
  if (UNLIKELY(interfaces_sfield.get() == NULL)) {
    CHECK(self->IsExceptionPending());  // OOME.
    return NULL;
  }
  klass->SetStaticField(0, interfaces_sfield.get());
  interfaces_sfield->SetDexFieldIndex(0);
  interfaces_sfield->SetDeclaringClass(klass.get());
  interfaces_sfield->SetAccessFlags(kAccStatic | kAccPublic | kAccFinal);
  // 2. Create a static field 'throws' that holds exceptions thrown by our methods.
  SirtRef<mirror::ArtField> throws_sfield(self, AllocArtField(self));
  if (UNLIKELY(throws_sfield.get() == NULL)) {
    CHECK(self->IsExceptionPending());  // OOME.
    return NULL;
  }
  klass->SetStaticField(1, throws_sfield.get());
  throws_sfield->SetDexFieldIndex(1);
  throws_sfield->SetDeclaringClass(klass.get());
  throws_sfield->SetAccessFlags(kAccStatic | kAccPublic | kAccFinal);

  // Proxies have 1 direct method, the constructor
  {
    mirror::ObjectArray<mirror::ArtMethod>* directs = AllocArtMethodArray(self, 1);
    if (UNLIKELY(directs == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return NULL;
    }
    klass->SetDirectMethods(directs);
    mirror::ArtMethod* constructor = CreateProxyConstructor(self, klass, proxy_class);
    if (UNLIKELY(constructor == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return NULL;
    }
    klass->SetDirectMethod(0, constructor);
  }

  // Create virtual method using specified prototypes.
  size_t num_virtual_methods =
      soa.Decode<mirror::ObjectArray<mirror::ArtMethod>*>(methods)->GetLength();
  {
    mirror::ObjectArray<mirror::ArtMethod>* virtuals = AllocArtMethodArray(self, num_virtual_methods);
    if (UNLIKELY(virtuals == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return NULL;
    }
    klass->SetVirtualMethods(virtuals);
  }
  for (size_t i = 0; i < num_virtual_methods; ++i) {
    mirror::ObjectArray<mirror::ArtMethod>* decoded_methods =
        soa.Decode<mirror::ObjectArray<mirror::ArtMethod>*>(methods);
    SirtRef<mirror::ArtMethod> prototype(self, decoded_methods->Get(i));
    mirror::ArtMethod* clone = CreateProxyMethod(self, klass, prototype);
    if (UNLIKELY(clone == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return NULL;
    }
    klass->SetVirtualMethod(i, clone);
  }

  klass->SetSuperClass(proxy_class);  // The super class is java.lang.reflect.Proxy
  klass->SetStatus(mirror::Class::kStatusLoaded, self);  // Class is now effectively in the loaded state
  self->AssertNoPendingException();

  {
    ObjectLock<mirror::Class> lock(self, &klass);  // Must hold lock on object when resolved.
    // Link the fields and virtual methods, creating vtable and iftables
    SirtRef<mirror::ObjectArray<mirror::Class> > sirt_interfaces(
        self, soa.Decode<mirror::ObjectArray<mirror::Class>*>(interfaces));
    if (!LinkClass(self, klass, sirt_interfaces)) {
      klass->SetStatus(mirror::Class::kStatusError, self);
      return nullptr;
    }

    interfaces_sfield->SetObject<false>(klass.get(), soa.Decode<mirror::ObjectArray<mirror::Class>*>(interfaces));
    throws_sfield->SetObject<false>(klass.get(), soa.Decode<mirror::ObjectArray<mirror::ObjectArray<mirror::Class> >*>(throws));
    klass->SetStatus(mirror::Class::kStatusInitialized, self);
  }

  // sanity checks
  if (kIsDebugBuild) {
    CHECK(klass->GetIFields() == NULL);
    CheckProxyConstructor(klass->GetDirectMethod(0));
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      mirror::ObjectArray<mirror::ArtMethod>* decoded_methods =
          soa.Decode<mirror::ObjectArray<mirror::ArtMethod>*>(methods);
      SirtRef<mirror::ArtMethod> prototype(self, decoded_methods->Get(i));
      CheckProxyMethod(klass->GetVirtualMethod(i), prototype);
    }

    mirror::String* decoded_name = soa.Decode<mirror::String*>(name);
    std::string interfaces_field_name(StringPrintf("java.lang.Class[] %s.interfaces",
                                                   decoded_name->ToModifiedUtf8().c_str()));
    CHECK_EQ(PrettyField(klass->GetStaticField(0)), interfaces_field_name);

    std::string throws_field_name(StringPrintf("java.lang.Class[][] %s.throws",
                                               decoded_name->ToModifiedUtf8().c_str()));
    CHECK_EQ(PrettyField(klass->GetStaticField(1)), throws_field_name);

    mirror::SynthesizedProxyClass* synth_proxy_class =
        down_cast<mirror::SynthesizedProxyClass*>(klass.get());
    CHECK_EQ(synth_proxy_class->GetInterfaces(), soa.Decode<mirror::ObjectArray<mirror::Class>*>(interfaces));
    CHECK_EQ(synth_proxy_class->GetThrows(), soa.Decode<mirror::ObjectArray<mirror::ObjectArray<mirror::Class> >*>(throws));
  }
  std::string descriptor(GetDescriptorForProxy(klass.get()));
  mirror::Class* existing = InsertClass(descriptor.c_str(), klass.get(), Hash(descriptor.c_str()));
  CHECK(existing == nullptr);
  return klass.get();
}

std::string ClassLinker::GetDescriptorForProxy(mirror::Class* proxy_class) {
  DCHECK(proxy_class->IsProxyClass());
  mirror::String* name = proxy_class->GetName();
  DCHECK(name != NULL);
  return DotToDescriptor(name->ToModifiedUtf8().c_str());
}

mirror::ArtMethod* ClassLinker::FindMethodForProxy(mirror::Class* proxy_class,
                                                   mirror::ArtMethod* proxy_method) {
  DCHECK(proxy_class->IsProxyClass());
  DCHECK(proxy_method->IsProxyMethod());
  // Locate the dex cache of the original interface/Object
  mirror::DexCache* dex_cache = NULL;
  {
    mirror::ObjectArray<mirror::Class>* resolved_types = proxy_method->GetDexCacheResolvedTypes();
    ReaderMutexLock mu(Thread::Current(), dex_lock_);
    for (auto dex_cache_tmp : dex_caches_) {
      if (dex_cache_tmp.second->GetResolvedTypes() == resolved_types) {
        dex_cache = dex_cache_tmp.second;
        break;
      }
    }
  }
  CHECK(dex_cache != NULL);
  uint32_t method_idx = proxy_method->GetDexMethodIndex();
  mirror::ArtMethod* resolved_method = dex_cache->GetResolvedMethod(method_idx);
  CHECK(resolved_method != NULL);
  return resolved_method;
}


mirror::ArtMethod* ClassLinker::CreateProxyConstructor(Thread* self,
                                                       const SirtRef<mirror::Class>& klass,
                                                       mirror::Class* proxy_class) {
  // Create constructor for Proxy that must initialize h
  mirror::ObjectArray<mirror::ArtMethod>* proxy_direct_methods =
      proxy_class->GetDirectMethods();
  CHECK_EQ(proxy_direct_methods->GetLength(), 16);
  mirror::ArtMethod* proxy_constructor = proxy_direct_methods->Get(2);
  // Clone the existing constructor of Proxy (our constructor would just invoke it so steal its
  // code_ too)
  mirror::ArtMethod* constructor =
      down_cast<mirror::ArtMethod*>(proxy_constructor->Clone(self));
  if (constructor == NULL) {
    CHECK(self->IsExceptionPending());  // OOME.
    return NULL;
  }
  // Make this constructor public and fix the class to be our Proxy version
  constructor->SetAccessFlags((constructor->GetAccessFlags() & ~kAccProtected) | kAccPublic);
  constructor->SetDeclaringClass(klass.get());
  return constructor;
}

static void CheckProxyConstructor(mirror::ArtMethod* constructor)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CHECK(constructor->IsConstructor());
  MethodHelper mh(constructor);
  CHECK_STREQ(mh.GetName(), "<init>");
  CHECK_STREQ(mh.GetSignature().ToString().c_str(), "(Ljava/lang/reflect/InvocationHandler;)V");
  DCHECK(constructor->IsPublic());
}

mirror::ArtMethod* ClassLinker::CreateProxyMethod(Thread* self,
                                                  const SirtRef<mirror::Class>& klass,
                                                  const SirtRef<mirror::ArtMethod>& prototype) {
  // Ensure prototype is in dex cache so that we can use the dex cache to look up the overridden
  // prototype method
  prototype->GetDeclaringClass()->GetDexCache()->SetResolvedMethod(prototype->GetDexMethodIndex(),
                                                                   prototype.get());
  // We steal everything from the prototype (such as DexCache, invoke stub, etc.) then specialize
  // as necessary
  mirror::ArtMethod* method = down_cast<mirror::ArtMethod*>(prototype->Clone(self));
  if (UNLIKELY(method == NULL)) {
    CHECK(self->IsExceptionPending());  // OOME.
    return NULL;
  }

  // Set class to be the concrete proxy class and clear the abstract flag, modify exceptions to
  // the intersection of throw exceptions as defined in Proxy
  method->SetDeclaringClass(klass.get());
  method->SetAccessFlags((method->GetAccessFlags() & ~kAccAbstract) | kAccFinal);

  // At runtime the method looks like a reference and argument saving method, clone the code
  // related parameters from this method.
  mirror::ArtMethod* refs_and_args =
      Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs);
  method->SetCoreSpillMask(refs_and_args->GetCoreSpillMask());
  method->SetFpSpillMask(refs_and_args->GetFpSpillMask());
  method->SetFrameSizeInBytes(refs_and_args->GetFrameSizeInBytes());
  method->SetEntryPointFromQuickCompiledCode(GetQuickProxyInvokeHandler());
  method->SetEntryPointFromPortableCompiledCode(GetPortableProxyInvokeHandler());
  method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);

  return method;
}

static void CheckProxyMethod(mirror::ArtMethod* method,
                             SirtRef<mirror::ArtMethod>& prototype)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Basic sanity
  CHECK(!prototype->IsFinal());
  CHECK(method->IsFinal());
  CHECK(!method->IsAbstract());

  // The proxy method doesn't have its own dex cache or dex file and so it steals those of its
  // interface prototype. The exception to this are Constructors and the Class of the Proxy itself.
  CHECK_EQ(prototype->GetDexCacheStrings(), method->GetDexCacheStrings());
  CHECK_EQ(prototype->GetDexCacheResolvedMethods(), method->GetDexCacheResolvedMethods());
  CHECK_EQ(prototype->GetDexCacheResolvedTypes(), method->GetDexCacheResolvedTypes());
  CHECK_EQ(prototype->GetDexMethodIndex(), method->GetDexMethodIndex());

  MethodHelper mh(method);
  MethodHelper mh2(prototype.get());
  CHECK_STREQ(mh.GetName(), mh2.GetName());
  CHECK_STREQ(mh.GetShorty(), mh2.GetShorty());
  // More complex sanity - via dex cache
  CHECK_EQ(mh.GetReturnType(), mh2.GetReturnType());
}

static bool CanWeInitializeClass(mirror::Class* klass, bool can_init_statics,
                                 bool can_init_parents)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (can_init_statics && can_init_parents) {
    return true;
  }
  if (!can_init_statics) {
    // Check if there's a class initializer.
    mirror::ArtMethod* clinit = klass->FindClassInitializer();
    if (clinit != NULL) {
      return false;
    }
    // Check if there are encoded static values needing initialization.
    if (klass->NumStaticFields() != 0) {
      ClassHelper kh(klass);
      const DexFile::ClassDef* dex_class_def = kh.GetClassDef();
      DCHECK(dex_class_def != NULL);
      if (dex_class_def->static_values_off_ != 0) {
        return false;
      }
    }
  }
  if (!klass->IsInterface() && klass->HasSuperClass()) {
    mirror::Class* super_class = klass->GetSuperClass();
    if (!can_init_parents && !super_class->IsInitialized()) {
      return false;
    } else {
      if (!CanWeInitializeClass(super_class, can_init_statics, can_init_parents)) {
        return false;
      }
    }
  }
  return true;
}

bool ClassLinker::IsInitialized() const {
  return init_done_;
}

bool ClassLinker::InitializeClass(const SirtRef<mirror::Class>& klass, bool can_init_statics,
                                  bool can_init_parents) {
  // see JLS 3rd edition, 12.4.2 "Detailed Initialization Procedure" for the locking protocol

  // Are we already initialized and therefore done?
  // Note: we differ from the JLS here as we don't do this under the lock, this is benign as
  // an initialized class will never change its state.
  if (klass->IsInitialized()) {
    return true;
  }

  // Fast fail if initialization requires a full runtime. Not part of the JLS.
  if (!CanWeInitializeClass(klass.get(), can_init_statics, can_init_parents)) {
    return false;
  }

  Thread* self = Thread::Current();
  uint64_t t0;
  {
    ObjectLock<mirror::Class> lock(self, &klass);

    // Re-check under the lock in case another thread initialized ahead of us.
    if (klass->IsInitialized()) {
      return true;
    }

    // Was the class already found to be erroneous? Done under the lock to match the JLS.
    if (klass->IsErroneous()) {
      ThrowEarlierClassFailure(klass.get());
      return false;
    }

    CHECK(klass->IsResolved()) << PrettyClass(klass.get()) << ": state=" << klass->GetStatus();

    if (!klass->IsVerified()) {
      VerifyClass(klass);
      if (!klass->IsVerified()) {
        // We failed to verify, expect either the klass to be erroneous or verification failed at
        // compile time.
        if (klass->IsErroneous()) {
          CHECK(self->IsExceptionPending());
        } else {
          CHECK(Runtime::Current()->IsCompiler());
          CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusRetryVerificationAtRuntime);
        }
        return false;
      }
    }

    // If the class is kStatusInitializing, either this thread is
    // initializing higher up the stack or another thread has beat us
    // to initializing and we need to wait. Either way, this
    // invocation of InitializeClass will not be responsible for
    // running <clinit> and will return.
    if (klass->GetStatus() == mirror::Class::kStatusInitializing) {
      // We caught somebody else in the act; was it us?
      if (klass->GetClinitThreadId() == self->GetTid()) {
        // Yes. That's fine. Return so we can continue initializing.
        return true;
      }
      // No. That's fine. Wait for another thread to finish initializing.
      return WaitForInitializeClass(klass, self, lock);
    }

    if (!ValidateSuperClassDescriptors(klass)) {
      klass->SetStatus(mirror::Class::kStatusError, self);
      return false;
    }

    CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusVerified) << PrettyClass(klass.get());

    // From here out other threads may observe that we're initializing and so changes of state
    // require the a notification.
    klass->SetClinitThreadId(self->GetTid());
    klass->SetStatus(mirror::Class::kStatusInitializing, self);

    t0 = NanoTime();
  }

  // Initialize super classes, must be done while initializing for the JLS.
  if (!klass->IsInterface() && klass->HasSuperClass()) {
    mirror::Class* super_class = klass->GetSuperClass();
    if (!super_class->IsInitialized()) {
      CHECK(!super_class->IsInterface());
      CHECK(can_init_parents);
      SirtRef<mirror::Class> sirt_super(self, super_class);
      bool super_initialized = InitializeClass(sirt_super, can_init_statics, true);
      if (!super_initialized) {
        // The super class was verified ahead of entering initializing, we should only be here if
        // the super class became erroneous due to initialization.
        CHECK(sirt_super->IsErroneous() && self->IsExceptionPending())
            << "Super class initialization failed for " << PrettyDescriptor(sirt_super.get())
            << " that has unexpected status " << sirt_super->GetStatus()
            << "\nPending exception:\n"
            << (self->GetException(NULL) != NULL ? self->GetException(NULL)->Dump() : "");
        ObjectLock<mirror::Class> lock(self, &klass);
        // Initialization failed because the super-class is erroneous.
        klass->SetStatus(mirror::Class::kStatusError, self);
        return false;
      }
    }
  }

  if (klass->NumStaticFields() > 0) {
    ClassHelper kh(klass.get());
    const DexFile::ClassDef* dex_class_def = kh.GetClassDef();
    CHECK(dex_class_def != NULL);
    const DexFile& dex_file = kh.GetDexFile();
    SirtRef<mirror::ClassLoader> class_loader(self, klass->GetClassLoader());
    SirtRef<mirror::DexCache> dex_cache(self, kh.GetDexCache());
    EncodedStaticFieldValueIterator it(dex_file, &dex_cache, &class_loader,
                                       this, *dex_class_def);
    if (it.HasNext()) {
      CHECK(can_init_statics);
      // We reordered the fields, so we need to be able to map the field indexes to the right fields.
      SafeMap<uint32_t, mirror::ArtField*> field_map;
      ConstructFieldMap(dex_file, *dex_class_def, klass.get(), field_map);
      for (size_t i = 0; it.HasNext(); i++, it.Next()) {
        if (Runtime::Current()->IsActiveTransaction()) {
          it.ReadValueToField<true>(field_map.Get(i));
        } else {
          it.ReadValueToField<false>(field_map.Get(i));
        }
      }
    }
  }

  mirror::ArtMethod* clinit = klass->FindClassInitializer();
  if (clinit != NULL) {
    CHECK(can_init_statics);
    JValue result;
    clinit->Invoke(self, NULL, 0, &result, "V");
  }

  uint64_t t1 = NanoTime();

  bool success = true;
  {
    ObjectLock<mirror::Class> lock(self, &klass);

    if (self->IsExceptionPending()) {
      WrapExceptionInInitializer();
      klass->SetStatus(mirror::Class::kStatusError, self);
      success = false;
    } else {
      RuntimeStats* global_stats = Runtime::Current()->GetStats();
      RuntimeStats* thread_stats = self->GetStats();
      ++global_stats->class_init_count;
      ++thread_stats->class_init_count;
      global_stats->class_init_time_ns += (t1 - t0);
      thread_stats->class_init_time_ns += (t1 - t0);
      // Set the class as initialized except if failed to initialize static fields.
      klass->SetStatus(mirror::Class::kStatusInitialized, self);
      if (VLOG_IS_ON(class_linker)) {
        ClassHelper kh(klass.get());
        LOG(INFO) << "Initialized class " << kh.GetDescriptor() << " from " << kh.GetLocation();
      }
      // Opportunistically set static method trampolines to their destination.
      FixupStaticTrampolines(klass.get());
    }
  }
  return success;
}

bool ClassLinker::WaitForInitializeClass(const SirtRef<mirror::Class>& klass, Thread* self,
                                         ObjectLock<mirror::Class>& lock)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  while (true) {
    self->AssertNoPendingException();
    CHECK(!klass->IsInitialized());
    lock.WaitIgnoringInterrupts();

    // When we wake up, repeat the test for init-in-progress.  If
    // there's an exception pending (only possible if
    // "interruptShouldThrow" was set), bail out.
    if (self->IsExceptionPending()) {
      WrapExceptionInInitializer();
      klass->SetStatus(mirror::Class::kStatusError, self);
      return false;
    }
    // Spurious wakeup? Go back to waiting.
    if (klass->GetStatus() == mirror::Class::kStatusInitializing) {
      continue;
    }
    if (klass->GetStatus() == mirror::Class::kStatusVerified && Runtime::Current()->IsCompiler()) {
      // Compile time initialization failed.
      return false;
    }
    if (klass->IsErroneous()) {
      // The caller wants an exception, but it was thrown in a
      // different thread.  Synthesize one here.
      ThrowNoClassDefFoundError("<clinit> failed for class %s; see exception in other thread",
                                PrettyDescriptor(klass.get()).c_str());
      return false;
    }
    if (klass->IsInitialized()) {
      return true;
    }
    LOG(FATAL) << "Unexpected class status. " << PrettyClass(klass.get()) << " is "
        << klass->GetStatus();
  }
  LOG(FATAL) << "Not Reached" << PrettyClass(klass.get());
}

bool ClassLinker::ValidateSuperClassDescriptors(const SirtRef<mirror::Class>& klass) {
  if (klass->IsInterface()) {
    return true;
  }
  // Begin with the methods local to the superclass.
  MethodHelper mh;
  MethodHelper super_mh;
  if (klass->HasSuperClass() &&
      klass->GetClassLoader() != klass->GetSuperClass()->GetClassLoader()) {
    for (int i = klass->GetSuperClass()->GetVTable()->GetLength() - 1; i >= 0; --i) {
      mh.ChangeMethod(klass->GetVTable()->GetWithoutChecks(i));
      super_mh.ChangeMethod(klass->GetSuperClass()->GetVTable()->GetWithoutChecks(i));
      bool is_override = mh.GetMethod() != super_mh.GetMethod();
      if (is_override && !mh.HasSameSignatureWithDifferentClassLoaders(&super_mh)) {
        ThrowLinkageError(klass.get(), "Class %s method %s resolves differently in superclass %s",
                          PrettyDescriptor(klass.get()).c_str(),
                          PrettyMethod(mh.GetMethod()).c_str(),
                          PrettyDescriptor(klass->GetSuperClass()).c_str());
        return false;
      }
    }
  }
  for (int32_t i = 0; i < klass->GetIfTableCount(); ++i) {
    if (klass->GetClassLoader() != klass->GetIfTable()->GetInterface(i)->GetClassLoader()) {
      uint32_t num_methods = klass->GetIfTable()->GetInterface(i)->NumVirtualMethods();
      for (uint32_t j = 0; j < num_methods; ++j) {
        mh.ChangeMethod(klass->GetIfTable()->GetMethodArray(i)->GetWithoutChecks(j));
        super_mh.ChangeMethod(klass->GetIfTable()->GetInterface(i)->GetVirtualMethod(j));
        bool is_override = mh.GetMethod() != super_mh.GetMethod();
        if (is_override && !mh.HasSameSignatureWithDifferentClassLoaders(&super_mh)) {
          ThrowLinkageError(klass.get(), "Class %s method %s resolves differently in interface %s",
                            PrettyDescriptor(klass.get()).c_str(),
                            PrettyMethod(mh.GetMethod()).c_str(),
                            PrettyDescriptor(klass->GetIfTable()->GetInterface(i)).c_str());
          return false;
        }
      }
    }
  }
  return true;
}

bool ClassLinker::EnsureInitialized(const SirtRef<mirror::Class>& c, bool can_init_fields,
                                    bool can_init_parents) {
  DCHECK(c.get() != NULL);
  if (c->IsInitialized()) {
    return true;
  }

  bool success = InitializeClass(c, can_init_fields, can_init_parents);
  if (!success) {
    if (can_init_fields && can_init_parents) {
      CHECK(Thread::Current()->IsExceptionPending()) << PrettyClass(c.get());
    }
  }
  return success;
}

void ClassLinker::ConstructFieldMap(const DexFile& dex_file, const DexFile::ClassDef& dex_class_def,
                                    mirror::Class* c,
                                    SafeMap<uint32_t, mirror::ArtField*>& field_map) {
  const byte* class_data = dex_file.GetClassData(dex_class_def);
  ClassDataItemIterator it(dex_file, class_data);
  Thread* self = Thread::Current();
  SirtRef<mirror::DexCache> dex_cache(self, c->GetDexCache());
  SirtRef<mirror::ClassLoader> class_loader(self, c->GetClassLoader());
  CHECK(!kMovingFields);
  for (size_t i = 0; it.HasNextStaticField(); i++, it.Next()) {
    field_map.Put(i, ResolveField(dex_file, it.GetMemberIndex(), dex_cache, class_loader, true));
  }
}

bool ClassLinker::LinkClass(Thread* self, const SirtRef<mirror::Class>& klass,
                            const SirtRef<mirror::ObjectArray<mirror::Class> >& interfaces) {
  CHECK_EQ(mirror::Class::kStatusLoaded, klass->GetStatus());
  if (!LinkSuperClass(klass)) {
    return false;
  }
  if (!LinkMethods(klass, interfaces)) {
    return false;
  }
  if (!LinkInstanceFields(klass)) {
    return false;
  }
  if (!LinkStaticFields(klass)) {
    return false;
  }
  CreateReferenceInstanceOffsets(klass);
  CreateReferenceStaticOffsets(klass);
  CHECK_EQ(mirror::Class::kStatusLoaded, klass->GetStatus());
  klass->SetStatus(mirror::Class::kStatusResolved, self);
  return true;
}

bool ClassLinker::LoadSuperAndInterfaces(const SirtRef<mirror::Class>& klass,
                                         const DexFile& dex_file) {
  CHECK_EQ(mirror::Class::kStatusIdx, klass->GetStatus());
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(klass->GetDexClassDefIndex());
  uint16_t super_class_idx = class_def.superclass_idx_;
  if (super_class_idx != DexFile::kDexNoIndex16) {
    mirror::Class* super_class = ResolveType(dex_file, super_class_idx, klass.get());
    if (super_class == NULL) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return false;
    }
    // Verify
    if (!klass->CanAccess(super_class)) {
      ThrowIllegalAccessError(klass.get(), "Class %s extended by class %s is inaccessible",
                              PrettyDescriptor(super_class).c_str(),
                              PrettyDescriptor(klass.get()).c_str());
      return false;
    }
    klass->SetSuperClass(super_class);
  }
  const DexFile::TypeList* interfaces = dex_file.GetInterfacesList(class_def);
  if (interfaces != NULL) {
    for (size_t i = 0; i < interfaces->Size(); i++) {
      uint16_t idx = interfaces->GetTypeItem(i).type_idx_;
      mirror::Class* interface = ResolveType(dex_file, idx, klass.get());
      if (interface == NULL) {
        DCHECK(Thread::Current()->IsExceptionPending());
        return false;
      }
      // Verify
      if (!klass->CanAccess(interface)) {
        // TODO: the RI seemed to ignore this in my testing.
        ThrowIllegalAccessError(klass.get(), "Interface %s implemented by class %s is inaccessible",
                                PrettyDescriptor(interface).c_str(),
                                PrettyDescriptor(klass.get()).c_str());
        return false;
      }
    }
  }
  // Mark the class as loaded.
  klass->SetStatus(mirror::Class::kStatusLoaded, NULL);
  return true;
}

bool ClassLinker::LinkSuperClass(const SirtRef<mirror::Class>& klass) {
  CHECK(!klass->IsPrimitive());
  mirror::Class* super = klass->GetSuperClass();
  if (klass.get() == GetClassRoot(kJavaLangObject)) {
    if (super != NULL) {
      ThrowClassFormatError(klass.get(), "java.lang.Object must not have a superclass");
      return false;
    }
    return true;
  }
  if (super == NULL) {
    ThrowLinkageError(klass.get(), "No superclass defined for class %s",
                      PrettyDescriptor(klass.get()).c_str());
    return false;
  }
  // Verify
  if (super->IsFinal() || super->IsInterface()) {
    ThrowIncompatibleClassChangeError(klass.get(), "Superclass %s of %s is %s",
                                      PrettyDescriptor(super).c_str(),
                                      PrettyDescriptor(klass.get()).c_str(),
                                      super->IsFinal() ? "declared final" : "an interface");
    return false;
  }
  if (!klass->CanAccess(super)) {
    ThrowIllegalAccessError(klass.get(), "Superclass %s is inaccessible to class %s",
                            PrettyDescriptor(super).c_str(),
                            PrettyDescriptor(klass.get()).c_str());
    return false;
  }

  // Inherit kAccClassIsFinalizable from the superclass in case this class doesn't override finalize.
  if (super->IsFinalizable()) {
    klass->SetFinalizable();
  }

  // Inherit reference flags (if any) from the superclass.
  int reference_flags = (super->GetAccessFlags() & kAccReferenceFlagsMask);
  if (reference_flags != 0) {
    klass->SetAccessFlags(klass->GetAccessFlags() | reference_flags);
  }
  // Disallow custom direct subclasses of java.lang.ref.Reference.
  if (init_done_ && super == GetClassRoot(kJavaLangRefReference)) {
    ThrowLinkageError(klass.get(),
                      "Class %s attempts to subclass java.lang.ref.Reference, which is not allowed",
                      PrettyDescriptor(klass.get()).c_str());
    return false;
  }

  if (kIsDebugBuild) {
    // Ensure super classes are fully resolved prior to resolving fields..
    while (super != NULL) {
      CHECK(super->IsResolved());
      super = super->GetSuperClass();
    }
  }
  return true;
}

// Populate the class vtable and itable. Compute return type indices.
bool ClassLinker::LinkMethods(const SirtRef<mirror::Class>& klass,
                              const SirtRef<mirror::ObjectArray<mirror::Class> >& interfaces) {
  if (klass->IsInterface()) {
    // No vtable.
    size_t count = klass->NumVirtualMethods();
    if (!IsUint(16, count)) {
      ThrowClassFormatError(klass.get(), "Too many methods on interface: %zd", count);
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      klass->GetVirtualMethodDuringLinking(i)->SetMethodIndex(i);
    }
    // Link interface method tables
    return LinkInterfaceMethods(klass, interfaces);
  } else {
    // Link virtual and interface method tables
    return LinkVirtualMethods(klass) && LinkInterfaceMethods(klass, interfaces);
  }
  return true;
}

bool ClassLinker::LinkVirtualMethods(const SirtRef<mirror::Class>& klass) {
  Thread* self = Thread::Current();
  if (klass->HasSuperClass()) {
    uint32_t max_count = klass->NumVirtualMethods() + klass->GetSuperClass()->GetVTable()->GetLength();
    size_t actual_count = klass->GetSuperClass()->GetVTable()->GetLength();
    CHECK_LE(actual_count, max_count);
    // TODO: do not assign to the vtable field until it is fully constructed.
    SirtRef<mirror::ObjectArray<mirror::ArtMethod> >
      vtable(self, klass->GetSuperClass()->GetVTable()->CopyOf(self, max_count));
    if (UNLIKELY(vtable.get() == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return false;
    }
    // See if any of our virtual methods override the superclass.
    for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
      mirror::ArtMethod* local_method = klass->GetVirtualMethodDuringLinking(i);
      MethodHelper local_mh(local_method);
      size_t j = 0;
      for (; j < actual_count; ++j) {
        mirror::ArtMethod* super_method = vtable->Get(j);
        MethodHelper super_mh(super_method);
        if (local_mh.HasSameNameAndSignature(&super_mh)) {
          if (klass->CanAccessMember(super_method->GetDeclaringClass(), super_method->GetAccessFlags())) {
            if (super_method->IsFinal()) {
              ThrowLinkageError(klass.get(), "Method %s overrides final method in class %s",
                                PrettyMethod(local_method).c_str(),
                                super_mh.GetDeclaringClassDescriptor());
              return false;
            }
            vtable->Set<false>(j, local_method);
            local_method->SetMethodIndex(j);
            break;
          } else {
            LOG(WARNING) << "Before Android 4.1, method " << PrettyMethod(local_method)
                         << " would have incorrectly overridden the package-private method in "
                         << PrettyDescriptor(super_mh.GetDeclaringClassDescriptor());
          }
        }
      }
      if (j == actual_count) {
        // Not overriding, append.
        vtable->Set<false>(actual_count, local_method);
        local_method->SetMethodIndex(actual_count);
        actual_count += 1;
      }
    }
    if (!IsUint(16, actual_count)) {
      ThrowClassFormatError(klass.get(), "Too many methods defined on class: %zd", actual_count);
      return false;
    }
    // Shrink vtable if possible
    CHECK_LE(actual_count, max_count);
    if (actual_count < max_count) {
      vtable.reset(vtable->CopyOf(self, actual_count));
      if (UNLIKELY(vtable.get() == NULL)) {
        CHECK(self->IsExceptionPending());  // OOME.
        return false;
      }
    }
    klass->SetVTable(vtable.get());
  } else {
    CHECK(klass.get() == GetClassRoot(kJavaLangObject));
    uint32_t num_virtual_methods = klass->NumVirtualMethods();
    if (!IsUint(16, num_virtual_methods)) {
      ThrowClassFormatError(klass.get(), "Too many methods: %d", num_virtual_methods);
      return false;
    }
    SirtRef<mirror::ObjectArray<mirror::ArtMethod> >
        vtable(self, AllocArtMethodArray(self, num_virtual_methods));
    if (UNLIKELY(vtable.get() == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return false;
    }
    for (size_t i = 0; i < num_virtual_methods; ++i) {
      mirror::ArtMethod* virtual_method = klass->GetVirtualMethodDuringLinking(i);
      vtable->Set<false>(i, virtual_method);
      virtual_method->SetMethodIndex(i & 0xFFFF);
    }
    klass->SetVTable(vtable.get());
  }
  return true;
}

bool ClassLinker::LinkInterfaceMethods(const SirtRef<mirror::Class>& klass,
                                       const SirtRef<mirror::ObjectArray<mirror::Class> >& interfaces) {
  // Set the imt table to be all conflicts by default.
  klass->SetImTable(Runtime::Current()->GetDefaultImt());
  size_t super_ifcount;
  if (klass->HasSuperClass()) {
    super_ifcount = klass->GetSuperClass()->GetIfTableCount();
  } else {
    super_ifcount = 0;
  }
  size_t ifcount = super_ifcount;
  uint32_t num_interfaces;
  {
    ClassHelper kh(klass.get());
    num_interfaces =
        interfaces.get() == nullptr ? kh.NumDirectInterfaces() : interfaces->GetLength();
    ifcount += num_interfaces;
    for (size_t i = 0; i < num_interfaces; i++) {
      mirror::Class* interface =
          interfaces.get() == nullptr ? kh.GetDirectInterface(i) : interfaces->Get(i);
      ifcount += interface->GetIfTableCount();
    }
  }
  if (ifcount == 0) {
    // Class implements no interfaces.
    DCHECK_EQ(klass->GetIfTableCount(), 0);
    DCHECK(klass->GetIfTable() == NULL);
    return true;
  }
  if (ifcount == super_ifcount) {
    // Class implements same interfaces as parent, are any of these not marker interfaces?
    bool has_non_marker_interface = false;
    mirror::IfTable* super_iftable = klass->GetSuperClass()->GetIfTable();
    for (size_t i = 0; i < ifcount; ++i) {
      if (super_iftable->GetMethodArrayCount(i) > 0) {
        has_non_marker_interface = true;
        break;
      }
    }
    if (!has_non_marker_interface) {
      // Class just inherits marker interfaces from parent so recycle parent's iftable.
      klass->SetIfTable(super_iftable);
      return true;
    }
  }
  Thread* self = Thread::Current();
  SirtRef<mirror::IfTable> iftable(self, AllocIfTable(self, ifcount));
  if (UNLIKELY(iftable.get() == NULL)) {
    CHECK(self->IsExceptionPending());  // OOME.
    return false;
  }
  if (super_ifcount != 0) {
    mirror::IfTable* super_iftable = klass->GetSuperClass()->GetIfTable();
    for (size_t i = 0; i < super_ifcount; i++) {
      mirror::Class* super_interface = super_iftable->GetInterface(i);
      iftable->SetInterface(i, super_interface);
    }
  }
  // Flatten the interface inheritance hierarchy.
  size_t idx = super_ifcount;
  for (size_t i = 0; i < num_interfaces; i++) {
    ClassHelper kh(klass.get());
    mirror::Class* interface =
        interfaces.get() == nullptr ? kh.GetDirectInterface(i) : interfaces->Get(i);
    DCHECK(interface != NULL);
    if (!interface->IsInterface()) {
      ClassHelper ih(interface);
      ThrowIncompatibleClassChangeError(klass.get(), "Class %s implements non-interface class %s",
                                        PrettyDescriptor(klass.get()).c_str(),
                                        PrettyDescriptor(ih.GetDescriptor()).c_str());
      return false;
    }
    // Check if interface is already in iftable
    bool duplicate = false;
    for (size_t j = 0; j < idx; j++) {
      mirror::Class* existing_interface = iftable->GetInterface(j);
      if (existing_interface == interface) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      // Add this non-duplicate interface.
      iftable->SetInterface(idx++, interface);
      // Add this interface's non-duplicate super-interfaces.
      for (int32_t j = 0; j < interface->GetIfTableCount(); j++) {
        mirror::Class* super_interface = interface->GetIfTable()->GetInterface(j);
        bool super_duplicate = false;
        for (size_t k = 0; k < idx; k++) {
          mirror::Class* existing_interface = iftable->GetInterface(k);
          if (existing_interface == super_interface) {
            super_duplicate = true;
            break;
          }
        }
        if (!super_duplicate) {
          iftable->SetInterface(idx++, super_interface);
        }
      }
    }
  }
  // Shrink iftable in case duplicates were found
  if (idx < ifcount) {
    iftable.reset(down_cast<mirror::IfTable*>(iftable->CopyOf(self, idx * mirror::IfTable::kMax)));
    if (UNLIKELY(iftable.get() == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return false;
    }
    ifcount = idx;
  } else {
    CHECK_EQ(idx, ifcount);
  }
  klass->SetIfTable(iftable.get());

  // If we're an interface, we don't need the vtable pointers, so we're done.
  if (klass->IsInterface()) {
    return true;
  }
  // Allocate imtable
  bool imtable_changed = false;
  SirtRef<mirror::ObjectArray<mirror::ArtMethod> > imtable(self, AllocArtMethodArray(self, kImtSize));
  if (UNLIKELY(imtable.get() == NULL)) {
    CHECK(self->IsExceptionPending());  // OOME.
    return false;
  }
  std::vector<mirror::ArtMethod*> miranda_list;
  for (size_t i = 0; i < ifcount; ++i) {
    size_t num_methods = iftable->GetInterface(i)->NumVirtualMethods();
    if (num_methods > 0) {
      SirtRef<mirror::ObjectArray<mirror::ArtMethod> >
          method_array(self, AllocArtMethodArray(self, num_methods));
      if (UNLIKELY(method_array.get() == nullptr)) {
        CHECK(self->IsExceptionPending());  // OOME.
        return false;
      }
      iftable->SetMethodArray(i, method_array.get());
      SirtRef<mirror::ObjectArray<mirror::ArtMethod> > vtable(self,
                                                              klass->GetVTableDuringLinking());
      for (size_t j = 0; j < num_methods; ++j) {
        mirror::ArtMethod* interface_method = iftable->GetInterface(i)->GetVirtualMethod(j);
        MethodHelper interface_mh(interface_method);
        int32_t k;
        // For each method listed in the interface's method list, find the
        // matching method in our class's method list.  We want to favor the
        // subclass over the superclass, which just requires walking
        // back from the end of the vtable.  (This only matters if the
        // superclass defines a private method and this class redefines
        // it -- otherwise it would use the same vtable slot.  In .dex files
        // those don't end up in the virtual method table, so it shouldn't
        // matter which direction we go.  We walk it backward anyway.)
        for (k = vtable->GetLength() - 1; k >= 0; --k) {
          mirror::ArtMethod* vtable_method = vtable->Get(k);
          MethodHelper vtable_mh(vtable_method);
          if (interface_mh.HasSameNameAndSignature(&vtable_mh)) {
            if (!vtable_method->IsAbstract() && !vtable_method->IsPublic()) {
              ThrowIllegalAccessError(klass.get(),
                                      "Method '%s' implementing interface method '%s' is not public",
                                      PrettyMethod(vtable_method).c_str(),
                                      PrettyMethod(interface_method).c_str());
              return false;
            }
            method_array->Set<false>(j, vtable_method);
            // Place method in imt if entry is empty, place conflict otherwise.
            uint32_t imt_index = interface_method->GetDexMethodIndex() % kImtSize;
            if (imtable->Get(imt_index) == NULL) {
              imtable->Set<false>(imt_index, vtable_method);
              imtable_changed = true;
            } else {
              imtable->Set<false>(imt_index, Runtime::Current()->GetImtConflictMethod());
            }
            break;
          }
        }
        if (k < 0) {
          SirtRef<mirror::ArtMethod> miranda_method(self, NULL);
          for (size_t mir = 0; mir < miranda_list.size(); mir++) {
            mirror::ArtMethod* mir_method = miranda_list[mir];
            MethodHelper vtable_mh(mir_method);
            if (interface_mh.HasSameNameAndSignature(&vtable_mh)) {
              miranda_method.reset(miranda_list[mir]);
              break;
            }
          }
          if (miranda_method.get() == NULL) {
            // Point the interface table at a phantom slot.
            miranda_method.reset(down_cast<mirror::ArtMethod*>(interface_method->Clone(self)));
            if (UNLIKELY(miranda_method.get() == NULL)) {
              CHECK(self->IsExceptionPending());  // OOME.
              return false;
            }
            // TODO: If a methods move then the miranda_list may hold stale references.
            miranda_list.push_back(miranda_method.get());
          }
          method_array->Set<false>(j, miranda_method.get());
        }
      }
    }
  }
  if (imtable_changed) {
    // Fill in empty entries in interface method table with conflict.
    mirror::ArtMethod* imt_conflict_method = Runtime::Current()->GetImtConflictMethod();
    for (size_t i = 0; i < kImtSize; i++) {
      if (imtable->Get(i) == NULL) {
        imtable->Set<false>(i, imt_conflict_method);
      }
    }
    klass->SetImTable(imtable.get());
  }
  if (!miranda_list.empty()) {
    int old_method_count = klass->NumVirtualMethods();
    int new_method_count = old_method_count + miranda_list.size();
    mirror::ObjectArray<mirror::ArtMethod>* virtuals;
    if (old_method_count == 0) {
      virtuals = AllocArtMethodArray(self, new_method_count);
    } else {
      virtuals = klass->GetVirtualMethods()->CopyOf(self, new_method_count);
    }
    if (UNLIKELY(virtuals == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return false;
    }
    klass->SetVirtualMethods(virtuals);

    SirtRef<mirror::ObjectArray<mirror::ArtMethod> >
        vtable(self, klass->GetVTableDuringLinking());
    CHECK(vtable.get() != NULL);
    int old_vtable_count = vtable->GetLength();
    int new_vtable_count = old_vtable_count + miranda_list.size();
    vtable.reset(vtable->CopyOf(self, new_vtable_count));
    if (UNLIKELY(vtable.get() == NULL)) {
      CHECK(self->IsExceptionPending());  // OOME.
      return false;
    }
    for (size_t i = 0; i < miranda_list.size(); ++i) {
      mirror::ArtMethod* method = miranda_list[i];
      // Leave the declaring class alone as type indices are relative to it
      method->SetAccessFlags(method->GetAccessFlags() | kAccMiranda);
      method->SetMethodIndex(0xFFFF & (old_vtable_count + i));
      klass->SetVirtualMethod(old_method_count + i, method);
      vtable->Set<false>(old_vtable_count + i, method);
    }
    // TODO: do not assign to the vtable field until it is fully constructed.
    klass->SetVTable(vtable.get());
  }

  mirror::ObjectArray<mirror::ArtMethod>* vtable = klass->GetVTableDuringLinking();
  for (int i = 0; i < vtable->GetLength(); ++i) {
    CHECK(vtable->Get(i) != NULL);
  }

//  klass->DumpClass(std::cerr, Class::kDumpClassFullDetail);

  return true;
}

bool ClassLinker::LinkInstanceFields(const SirtRef<mirror::Class>& klass) {
  CHECK(klass.get() != NULL);
  return LinkFields(klass, false);
}

bool ClassLinker::LinkStaticFields(const SirtRef<mirror::Class>& klass) {
  CHECK(klass.get() != NULL);
  size_t allocated_class_size = klass->GetClassSize();
  bool success = LinkFields(klass, true);
  CHECK_EQ(allocated_class_size, klass->GetClassSize());
  return success;
}

struct LinkFieldsComparator {
  explicit LinkFieldsComparator() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  }
  // No thread safety analysis as will be called from STL. Checked lock held in constructor.
  bool operator()(mirror::ArtField* field1, mirror::ArtField* field2)
      NO_THREAD_SAFETY_ANALYSIS {
    // First come reference fields, then 64-bit, and finally 32-bit
    FieldHelper fh1(field1);
    Primitive::Type type1 = fh1.GetTypeAsPrimitiveType();
    FieldHelper fh2(field2);
    Primitive::Type type2 = fh2.GetTypeAsPrimitiveType();
    if (type1 != type2) {
      bool is_primitive1 = type1 != Primitive::kPrimNot;
      bool is_primitive2 = type2 != Primitive::kPrimNot;
      bool is64bit1 = is_primitive1 && (type1 == Primitive::kPrimLong || type1 == Primitive::kPrimDouble);
      bool is64bit2 = is_primitive2 && (type2 == Primitive::kPrimLong || type2 == Primitive::kPrimDouble);
      int order1 = !is_primitive1 ? 0 : (is64bit1 ? 1 : 2);
      int order2 = !is_primitive2 ? 0 : (is64bit2 ? 1 : 2);
      if (order1 != order2) {
        return order1 < order2;
      }
    }
    // same basic group? then sort by string.
    const char* name1 = fh1.GetName();
    const char* name2 = fh2.GetName();
    return strcmp(name1, name2) < 0;
  }
};

bool ClassLinker::LinkFields(const SirtRef<mirror::Class>& klass, bool is_static) {
  size_t num_fields =
      is_static ? klass->NumStaticFields() : klass->NumInstanceFields();

  mirror::ObjectArray<mirror::ArtField>* fields =
      is_static ? klass->GetSFields() : klass->GetIFields();

  // Initialize size and field_offset
  size_t size;
  MemberOffset field_offset(0);
  if (is_static) {
    size = klass->GetClassSize();
    field_offset = mirror::Class::FieldsOffset();
  } else {
    mirror::Class* super_class = klass->GetSuperClass();
    if (super_class != NULL) {
      CHECK(super_class->IsResolved());
      field_offset = MemberOffset(super_class->GetObjectSize());
    }
    size = field_offset.Uint32Value();
  }

  CHECK_EQ(num_fields == 0, fields == NULL);

  // we want a relatively stable order so that adding new fields
  // minimizes disruption of C++ version such as Class and Method.
  std::deque<mirror::ArtField*> grouped_and_sorted_fields;
  for (size_t i = 0; i < num_fields; i++) {
    mirror::ArtField* f = fields->Get(i);
    CHECK(f != NULL);
    grouped_and_sorted_fields.push_back(f);
  }
  std::sort(grouped_and_sorted_fields.begin(), grouped_and_sorted_fields.end(),
            LinkFieldsComparator());

  // References should be at the front.
  size_t current_field = 0;
  size_t num_reference_fields = 0;
  for (; current_field < num_fields; current_field++) {
    mirror::ArtField* field = grouped_and_sorted_fields.front();
    FieldHelper fh(field);
    Primitive::Type type = fh.GetTypeAsPrimitiveType();
    bool isPrimitive = type != Primitive::kPrimNot;
    if (isPrimitive) {
      break;  // past last reference, move on to the next phase
    }
    grouped_and_sorted_fields.pop_front();
    num_reference_fields++;
    fields->Set<false>(current_field, field);
    field->SetOffset(field_offset);
    field_offset = MemberOffset(field_offset.Uint32Value() + sizeof(uint32_t));
  }

  // Now we want to pack all of the double-wide fields together.  If
  // we're not aligned, though, we want to shuffle one 32-bit field
  // into place.  If we can't find one, we'll have to pad it.
  if (current_field != num_fields && !IsAligned<8>(field_offset.Uint32Value())) {
    for (size_t i = 0; i < grouped_and_sorted_fields.size(); i++) {
      mirror::ArtField* field = grouped_and_sorted_fields[i];
      FieldHelper fh(field);
      Primitive::Type type = fh.GetTypeAsPrimitiveType();
      CHECK(type != Primitive::kPrimNot);  // should only be working on primitive types
      if (type == Primitive::kPrimLong || type == Primitive::kPrimDouble) {
        continue;
      }
      fields->Set<false>(current_field++, field);
      field->SetOffset(field_offset);
      // drop the consumed field
      grouped_and_sorted_fields.erase(grouped_and_sorted_fields.begin() + i);
      break;
    }
    // whether we found a 32-bit field for padding or not, we advance
    field_offset = MemberOffset(field_offset.Uint32Value() + sizeof(uint32_t));
  }

  // Alignment is good, shuffle any double-wide fields forward, and
  // finish assigning field offsets to all fields.
  DCHECK(current_field == num_fields || IsAligned<8>(field_offset.Uint32Value()));
  while (!grouped_and_sorted_fields.empty()) {
    mirror::ArtField* field = grouped_and_sorted_fields.front();
    grouped_and_sorted_fields.pop_front();
    FieldHelper fh(field);
    Primitive::Type type = fh.GetTypeAsPrimitiveType();
    CHECK(type != Primitive::kPrimNot);  // should only be working on primitive types
    fields->Set<false>(current_field, field);
    field->SetOffset(field_offset);
    field_offset = MemberOffset(field_offset.Uint32Value() +
                                ((type == Primitive::kPrimLong || type == Primitive::kPrimDouble)
                                 ? sizeof(uint64_t)
                                 : sizeof(uint32_t)));
    current_field++;
  }

  // We lie to the GC about the java.lang.ref.Reference.referent field, so it doesn't scan it.
  if (!is_static &&
      (strcmp("Ljava/lang/ref/Reference;", ClassHelper(klass.get()).GetDescriptor()) == 0)) {
    // We know there are no non-reference fields in the Reference classes, and we know
    // that 'referent' is alphabetically last, so this is easy...
    CHECK_EQ(num_reference_fields, num_fields);
    FieldHelper fh(fields->Get(num_fields - 1));
    CHECK_STREQ(fh.GetName(), "referent");
    --num_reference_fields;
  }

  if (kIsDebugBuild) {
    // Make sure that all reference fields appear before
    // non-reference fields, and all double-wide fields are aligned.
    bool seen_non_ref = false;
    for (size_t i = 0; i < num_fields; i++) {
      mirror::ArtField* field = fields->Get(i);
      if (false) {  // enable to debug field layout
        LOG(INFO) << "LinkFields: " << (is_static ? "static" : "instance")
                    << " class=" << PrettyClass(klass.get())
                    << " field=" << PrettyField(field)
                    << " offset="
                    << field->GetField32(MemberOffset(mirror::ArtField::OffsetOffset()));
      }
      FieldHelper fh(field);
      Primitive::Type type = fh.GetTypeAsPrimitiveType();
      bool is_primitive = type != Primitive::kPrimNot;
      if ((strcmp("Ljava/lang/ref/Reference;", ClassHelper(klass.get()).GetDescriptor()) == 0)
          && (strcmp("referent", fh.GetName()) == 0)) {
        is_primitive = true;  // We lied above, so we have to expect a lie here.
      }
      if (is_primitive) {
        if (!seen_non_ref) {
          seen_non_ref = true;
          DCHECK_EQ(num_reference_fields, i);
        }
      } else {
        DCHECK(!seen_non_ref);
      }
    }
    if (!seen_non_ref) {
      DCHECK_EQ(num_fields, num_reference_fields);
    }
  }
  size = field_offset.Uint32Value();
  // Update klass
  if (is_static) {
    klass->SetNumReferenceStaticFields(num_reference_fields);
    klass->SetClassSize(size);
  } else {
    klass->SetNumReferenceInstanceFields(num_reference_fields);
    if (!klass->IsVariableSize()) {
      DCHECK_GE(size, sizeof(mirror::Object)) << ClassHelper(klass.get()).GetDescriptor();
      size_t previous_size = klass->GetObjectSize();
      if (previous_size != 0) {
        // Make sure that we didn't originally have an incorrect size.
        CHECK_EQ(previous_size, size);
      }
      klass->SetObjectSize(size);
    }
  }
  return true;
}

//  Set the bitmap of reference offsets, refOffsets, from the ifields
//  list.
void ClassLinker::CreateReferenceInstanceOffsets(const SirtRef<mirror::Class>& klass) {
  uint32_t reference_offsets = 0;
  mirror::Class* super_class = klass->GetSuperClass();
  if (super_class != NULL) {
    reference_offsets = super_class->GetReferenceInstanceOffsets();
    // If our superclass overflowed, we don't stand a chance.
    if (reference_offsets == CLASS_WALK_SUPER) {
      klass->SetReferenceInstanceOffsets(reference_offsets);
      return;
    }
  }
  CreateReferenceOffsets(klass, false, reference_offsets);
}

void ClassLinker::CreateReferenceStaticOffsets(const SirtRef<mirror::Class>& klass) {
  CreateReferenceOffsets(klass, true, 0);
}

void ClassLinker::CreateReferenceOffsets(const SirtRef<mirror::Class>& klass, bool is_static,
                                         uint32_t reference_offsets) {
  size_t num_reference_fields =
      is_static ? klass->NumReferenceStaticFieldsDuringLinking()
                : klass->NumReferenceInstanceFieldsDuringLinking();
  mirror::ObjectArray<mirror::ArtField>* fields =
      is_static ? klass->GetSFields() : klass->GetIFields();
  // All of the fields that contain object references are guaranteed
  // to be at the beginning of the fields list.
  for (size_t i = 0; i < num_reference_fields; ++i) {
    // Note that byte_offset is the offset from the beginning of
    // object, not the offset into instance data
    mirror::ArtField* field = fields->Get(i);
    MemberOffset byte_offset = field->GetOffsetDuringLinking();
    CHECK_EQ(byte_offset.Uint32Value() & (CLASS_OFFSET_ALIGNMENT - 1), 0U);
    if (CLASS_CAN_ENCODE_OFFSET(byte_offset.Uint32Value())) {
      uint32_t new_bit = CLASS_BIT_FROM_OFFSET(byte_offset.Uint32Value());
      CHECK_NE(new_bit, 0U);
      reference_offsets |= new_bit;
    } else {
      reference_offsets = CLASS_WALK_SUPER;
      break;
    }
  }
  // Update fields in klass
  if (is_static) {
    klass->SetReferenceStaticOffsets(reference_offsets);
  } else {
    klass->SetReferenceInstanceOffsets(reference_offsets);
  }
}

mirror::String* ClassLinker::ResolveString(const DexFile& dex_file, uint32_t string_idx,
                                           const SirtRef<mirror::DexCache>& dex_cache) {
  DCHECK(dex_cache.get() != nullptr);
  mirror::String* resolved = dex_cache->GetResolvedString(string_idx);
  if (resolved != NULL) {
    return resolved;
  }
  uint32_t utf16_length;
  const char* utf8_data = dex_file.StringDataAndUtf16LengthByIdx(string_idx, &utf16_length);
  mirror::String* string = intern_table_->InternStrong(utf16_length, utf8_data);
  dex_cache->SetResolvedString(string_idx, string);
  return string;
}

mirror::Class* ClassLinker::ResolveType(const DexFile& dex_file, uint16_t type_idx,
                                        mirror::Class* referrer) {
  Thread* self = Thread::Current();
  SirtRef<mirror::DexCache> dex_cache(self, referrer->GetDexCache());
  SirtRef<mirror::ClassLoader> class_loader(self, referrer->GetClassLoader());
  return ResolveType(dex_file, type_idx, dex_cache, class_loader);
}

mirror::Class* ClassLinker::ResolveType(const DexFile& dex_file, uint16_t type_idx,
                                        const SirtRef<mirror::DexCache>& dex_cache,
                                        const SirtRef<mirror::ClassLoader>& class_loader) {
  DCHECK(dex_cache.get() != NULL);
  mirror::Class* resolved = dex_cache->GetResolvedType(type_idx);
  if (resolved == NULL) {
    Thread* self = Thread::Current();
    const char* descriptor = dex_file.StringByTypeIdx(type_idx);
    resolved = FindClass(self, descriptor, class_loader);
    if (resolved != NULL) {
      // TODO: we used to throw here if resolved's class loader was not the
      //       boot class loader. This was to permit different classes with the
      //       same name to be loaded simultaneously by different loaders
      dex_cache->SetResolvedType(type_idx, resolved);
    } else {
      CHECK(self->IsExceptionPending())
          << "Expected pending exception for failed resolution of: " << descriptor;
      // Convert a ClassNotFoundException to a NoClassDefFoundError.
      SirtRef<mirror::Throwable> cause(self, self->GetException(NULL));
      if (cause->InstanceOf(GetClassRoot(kJavaLangClassNotFoundException))) {
        DCHECK(resolved == NULL);  // No SirtRef needed to preserve resolved.
        self->ClearException();
        ThrowNoClassDefFoundError("Failed resolution of: %s", descriptor);
        self->GetException(NULL)->SetCause(cause.get());
      }
    }
  }
  DCHECK((resolved == NULL) || resolved->IsResolved() || resolved->IsErroneous())
          << PrettyDescriptor(resolved) << " " << resolved->GetStatus();
  return resolved;
}

mirror::ArtMethod* ClassLinker::ResolveMethod(const DexFile& dex_file,
                                              uint32_t method_idx,
                                              const SirtRef<mirror::DexCache>& dex_cache,
                                              const SirtRef<mirror::ClassLoader>& class_loader,
                                              mirror::ArtMethod* referrer,
                                              InvokeType type) {
  DCHECK(dex_cache.get() != NULL);
  // Check for hit in the dex cache.
  mirror::ArtMethod* resolved = dex_cache->GetResolvedMethod(method_idx);
  if (resolved != NULL && !resolved->IsRuntimeMethod()) {
    return resolved;
  }
  // Fail, get the declaring class.
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  mirror::Class* klass = ResolveType(dex_file, method_id.class_idx_, dex_cache, class_loader);
  if (klass == NULL) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }
  // Scan using method_idx, this saves string compares but will only hit for matching dex
  // caches/files.
  switch (type) {
    case kDirect:  // Fall-through.
    case kStatic:
      resolved = klass->FindDirectMethod(dex_cache.get(), method_idx);
      break;
    case kInterface:
      resolved = klass->FindInterfaceMethod(dex_cache.get(), method_idx);
      DCHECK(resolved == NULL || resolved->GetDeclaringClass()->IsInterface());
      break;
    case kSuper:  // Fall-through.
    case kVirtual:
      resolved = klass->FindVirtualMethod(dex_cache.get(), method_idx);
      break;
    default:
      LOG(FATAL) << "Unreachable - invocation type: " << type;
  }
  if (resolved == NULL) {
    // Search by name, which works across dex files.
    const char* name = dex_file.StringDataByIdx(method_id.name_idx_);
    const Signature signature = dex_file.GetMethodSignature(method_id);
    switch (type) {
      case kDirect:  // Fall-through.
      case kStatic:
        resolved = klass->FindDirectMethod(name, signature);
        break;
      case kInterface:
        resolved = klass->FindInterfaceMethod(name, signature);
        DCHECK(resolved == NULL || resolved->GetDeclaringClass()->IsInterface());
        break;
      case kSuper:  // Fall-through.
      case kVirtual:
        resolved = klass->FindVirtualMethod(name, signature);
        break;
    }
  }
  if (resolved != NULL) {
    // We found a method, check for incompatible class changes.
    if (resolved->CheckIncompatibleClassChange(type)) {
      resolved = NULL;
    }
  }
  if (resolved != NULL) {
    // Be a good citizen and update the dex cache to speed subsequent calls.
    dex_cache->SetResolvedMethod(method_idx, resolved);
    return resolved;
  } else {
    // We failed to find the method which means either an access error, an incompatible class
    // change, or no such method. First try to find the method among direct and virtual methods.
    const char* name = dex_file.StringDataByIdx(method_id.name_idx_);
    const Signature signature = dex_file.GetMethodSignature(method_id);
    switch (type) {
      case kDirect:
      case kStatic:
        resolved = klass->FindVirtualMethod(name, signature);
        break;
      case kInterface:
      case kVirtual:
      case kSuper:
        resolved = klass->FindDirectMethod(name, signature);
        break;
    }

    // If we found something, check that it can be accessed by the referrer.
    if (resolved != NULL && referrer != NULL) {
      mirror::Class* methods_class = resolved->GetDeclaringClass();
      mirror::Class* referring_class = referrer->GetDeclaringClass();
      if (!referring_class->CanAccess(methods_class)) {
        ThrowIllegalAccessErrorClassForMethodDispatch(referring_class, methods_class,
                                                      resolved, type);
        return NULL;
      } else if (!referring_class->CanAccessMember(methods_class,
                                                   resolved->GetAccessFlags())) {
        ThrowIllegalAccessErrorMethod(referring_class, resolved);
        return NULL;
      }
    }

    // Otherwise, throw an IncompatibleClassChangeError if we found something, and check interface
    // methods and throw if we find the method there. If we find nothing, throw a NoSuchMethodError.
    switch (type) {
      case kDirect:
      case kStatic:
        if (resolved != NULL) {
          ThrowIncompatibleClassChangeError(type, kVirtual, resolved, referrer);
        } else {
          resolved = klass->FindInterfaceMethod(name, signature);
          if (resolved != NULL) {
            ThrowIncompatibleClassChangeError(type, kInterface, resolved, referrer);
          } else {
            ThrowNoSuchMethodError(type, klass, name, signature);
          }
        }
        break;
      case kInterface:
        if (resolved != NULL) {
          ThrowIncompatibleClassChangeError(type, kDirect, resolved, referrer);
        } else {
          resolved = klass->FindVirtualMethod(name, signature);
          if (resolved != NULL) {
            ThrowIncompatibleClassChangeError(type, kVirtual, resolved, referrer);
          } else {
            ThrowNoSuchMethodError(type, klass, name, signature);
          }
        }
        break;
      case kSuper:
        ThrowNoSuchMethodError(type, klass, name, signature);
        break;
      case kVirtual:
        if (resolved != NULL) {
          ThrowIncompatibleClassChangeError(type, kDirect, resolved, referrer);
        } else {
          resolved = klass->FindInterfaceMethod(name, signature);
          if (resolved != NULL) {
            ThrowIncompatibleClassChangeError(type, kInterface, resolved, referrer);
          } else {
            ThrowNoSuchMethodError(type, klass, name, signature);
          }
        }
        break;
    }
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }
}

mirror::ArtField* ClassLinker::ResolveField(const DexFile& dex_file, uint32_t field_idx,
                                            const SirtRef<mirror::DexCache>& dex_cache,
                                            const SirtRef<mirror::ClassLoader>& class_loader,
                                            bool is_static) {
  DCHECK(dex_cache.get() != nullptr);
  mirror::ArtField* resolved = dex_cache->GetResolvedField(field_idx);
  if (resolved != NULL) {
    return resolved;
  }
  const DexFile::FieldId& field_id = dex_file.GetFieldId(field_idx);
  mirror::Class* klass = ResolveType(dex_file, field_id.class_idx_, dex_cache, class_loader);
  if (klass == NULL) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }

  if (is_static) {
    resolved = klass->FindStaticField(dex_cache.get(), field_idx);
  } else {
    resolved = klass->FindInstanceField(dex_cache.get(), field_idx);
  }

  if (resolved == NULL) {
    const char* name = dex_file.GetFieldName(field_id);
    const char* type = dex_file.GetFieldTypeDescriptor(field_id);
    if (is_static) {
      resolved = klass->FindStaticField(name, type);
    } else {
      resolved = klass->FindInstanceField(name, type);
    }
    if (resolved == NULL) {
      ThrowNoSuchFieldError(is_static ? "static " : "instance ", klass, type, name);
      return NULL;
    }
  }
  dex_cache->SetResolvedField(field_idx, resolved);
  return resolved;
}

mirror::ArtField* ClassLinker::ResolveFieldJLS(const DexFile& dex_file,
                                               uint32_t field_idx,
                                               const SirtRef<mirror::DexCache>& dex_cache,
                                               const SirtRef<mirror::ClassLoader>& class_loader) {
  DCHECK(dex_cache.get() != nullptr);
  mirror::ArtField* resolved = dex_cache->GetResolvedField(field_idx);
  if (resolved != NULL) {
    return resolved;
  }
  const DexFile::FieldId& field_id = dex_file.GetFieldId(field_idx);
  mirror::Class* klass = ResolveType(dex_file, field_id.class_idx_, dex_cache, class_loader);
  if (klass == NULL) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }

  StringPiece name(dex_file.StringDataByIdx(field_id.name_idx_));
  StringPiece type(dex_file.StringDataByIdx(
      dex_file.GetTypeId(field_id.type_idx_).descriptor_idx_));
  resolved = klass->FindField(name, type);
  if (resolved != NULL) {
    dex_cache->SetResolvedField(field_idx, resolved);
  } else {
    ThrowNoSuchFieldError("", klass, type, name);
  }
  return resolved;
}

const char* ClassLinker::MethodShorty(uint32_t method_idx, mirror::ArtMethod* referrer,
                                      uint32_t* length) {
  mirror::Class* declaring_class = referrer->GetDeclaringClass();
  mirror::DexCache* dex_cache = declaring_class->GetDexCache();
  const DexFile& dex_file = *dex_cache->GetDexFile();
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  return dex_file.GetMethodShorty(method_id, length);
}

void ClassLinker::DumpAllClasses(int flags) {
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  // TODO: at the time this was written, it wasn't safe to call PrettyField with the ClassLinker
  // lock held, because it might need to resolve a field's type, which would try to take the lock.
  std::vector<mirror::Class*> all_classes;
  {
    ReaderMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
    for (const std::pair<size_t, mirror::Class*>& it : class_table_) {
      all_classes.push_back(it.second);
    }
  }

  for (size_t i = 0; i < all_classes.size(); ++i) {
    all_classes[i]->DumpClass(std::cerr, flags);
  }
}

void ClassLinker::DumpForSigQuit(std::ostream& os) {
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  ReaderMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  os << "Loaded classes: " << class_table_.size() << " allocated classes\n";
}

size_t ClassLinker::NumLoadedClasses() {
  if (dex_cache_image_class_lookup_required_) {
    MoveImageClassesToClassTable();
  }
  ReaderMutexLock mu(Thread::Current(), *Locks::classlinker_classes_lock_);
  return class_table_.size();
}

pid_t ClassLinker::GetClassesLockOwner() {
  return Locks::classlinker_classes_lock_->GetExclusiveOwnerTid();
}

pid_t ClassLinker::GetDexLockOwner() {
  return dex_lock_.GetExclusiveOwnerTid();
}

void ClassLinker::SetClassRoot(ClassRoot class_root, mirror::Class* klass) {
  DCHECK(!init_done_);

  DCHECK(klass != NULL);
  DCHECK(klass->GetClassLoader() == NULL);

  DCHECK(class_roots_ != NULL);
  DCHECK(class_roots_->Get(class_root) == NULL);
  class_roots_->Set<false>(class_root, klass);
}

}  // namespace art
