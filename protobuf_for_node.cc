// Copyright 2010 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you
// may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.

#include "protobuf_for_node.h"

#include <map>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>

#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/service.h>

#include <node_buffer.h>
#include <node_object_wrap.h>

using google::protobuf::Descriptor;
using google::protobuf::DescriptorPool;
using google::protobuf::DynamicMessageFactory;
using google::protobuf::FieldDescriptor;
using google::protobuf::FileDescriptorSet;
using google::protobuf::Message;
using google::protobuf::MethodDescriptor;
using google::protobuf::Reflection;
using google::protobuf::Service;
using google::protobuf::ServiceDescriptor;

using node::ObjectWrap;
using node::Buffer;

using std::map;
using std::string;
using std::vector;
using std::cerr;
using std::endl;

using v8::Array;
using v8::AccessorInfo;
using v8::Arguments;
using v8::Boolean;
using v8::Context;
using v8::External;
using v8::Function;
using v8::FunctionTemplate;
using v8::Integer;
using v8::Handle;
using v8::HandleScope;
using v8::InvocationCallback;
using v8::Local;
using v8::NamedPropertyGetter;
using v8::Number;
using v8::Object;
using v8::ObjectTemplate;
using v8::Persistent;
using v8::Script;
using v8::String;
using v8::Value;
using v8::V8;

namespace protobuf_for_node {
  template <typename T>
  static T* UnwrapThis(const Arguments& args) {
    return ObjectWrap::Unwrap<T>(args.This());
  }

  template <typename T>
  static T* UnwrapThis(const AccessorInfo& args) {
    return ObjectWrap::Unwrap<T>(args.This());
  }

  template <typename T, typename U>
  Handle<T> As(Handle<U> handle) {
    return Handle<T>::Cast(handle);
  }

  Persistent<FunctionTemplate> SchemaTemplate;
  Persistent<FunctionTemplate> ServiceSchemaTemplate;
  Persistent<FunctionTemplate> ServiceTemplate;
  Persistent<FunctionTemplate> MethodTemplate;
  Persistent<FunctionTemplate> TypeTemplate;
  Persistent<FunctionTemplate> ParseTemplate;
  Persistent<FunctionTemplate> SerializeTemplate;

  class Schema : public ObjectWrap {
  public:
    Schema(Handle<Object> self, const DescriptorPool* pool)
        : pool_(pool) {
      factory_.SetDelegateToGeneratedFactory(true);
      self->SetInternalField(1, Array::New());
      Wrap(self);
    }

    virtual ~Schema() {
      if (pool_ != DescriptorPool::generated_pool())
	delete pool_;
    }

    class Type : public ObjectWrap {
    public:
      Schema* schema_;
      const Descriptor* descriptor_;

      Message* NewMessage() const {
	return schema_->NewMessage(descriptor_);
      }

      Handle<Function> Constructor() const {
        return As<Function>(handle_->GetInternalField(2));
      }

      Local<Object> NewObject(Handle<Value> properties) const {
	return Constructor()->NewInstance(1, &properties);
      }

      Type(Schema* schema, const Descriptor* descriptor, Handle<Object> self)
	: schema_(schema), descriptor_(descriptor) {
	// Generate functions for bulk conversion between a JS object
	// and an array in descriptor order:
	//   from = function(arr) { this.f0 = arr[0]; this.f1 = arr[1]; ... }
	//   to   = function()    { return [ this.f0, this.f1, ... ] }
	// This is faster than repeatedly calling Get/Set on a v8::Object.
	std::ostringstream from, to;
	from << "(function(arr) { if(arr) {";
	to << "(function() { return [ ";

	for (int i = 0; i < descriptor->field_count(); i++) {
	  from <<
	    "var x = arr[" << i << "]; "
	    "if(x !== undefined) this['" <<
	    descriptor->field(i)->camelcase_name() <<
	    "'] = x; ";

	  if (i > 0) to << ", ";
	  to << "this['" << descriptor->field(i)->camelcase_name() << "']";
	}

	from << " }})";
	to << " ]; })";

        // managed type->schema link
	self->SetInternalField(1, schema_->handle_);

        Handle<Function> constructor = As<Function>(
            Script::Compile(String::New(from.str().c_str()))->Run());
        constructor->SetHiddenValue(String::New("type"), self);
	
	Handle<Function> bind =
	  As<Function>(Script::Compile(String::New("(function(self) { var f = this; return function(arg) { return f.call(self, arg); }; })"))->Run());
	Handle<Value> arg = self;
	constructor->Set(String::New("parse"), bind->Call(ParseTemplate->GetFunction(), 1, &arg));
	constructor->Set(String::New("serialize"), bind->Call(SerializeTemplate->GetFunction(), 1, &arg));
	self->SetInternalField(2, constructor);
	self->SetInternalField(3, Script::Compile(String::New(to.str().c_str()))->Run());

	Wrap(self);
      }

#define GET(TYPE)						\
      (index >= 0 ?						\
       reflection->GetRepeated##TYPE(instance, field, index) :	\
       reflection->Get##TYPE(instance, field))

      static Handle<Value> ToJs(const Message& instance,
                                const Reflection* reflection,
                                const FieldDescriptor* field,
                                const Type* message_type,
                                int index) {
	switch (field->cpp_type()) {
        case FieldDescriptor::CPPTYPE_MESSAGE:
          return message_type->ToJs(GET(Message));
        case FieldDescriptor::CPPTYPE_STRING: {
          const string& value = GET(String);
          return String::New(value.data(), value.length());
        }
        case FieldDescriptor::CPPTYPE_INT32:
          return Integer::New(GET(Int32));
        case FieldDescriptor::CPPTYPE_UINT32:
          return Integer::NewFromUnsigned(GET(UInt32));
        case FieldDescriptor::CPPTYPE_INT64:
          return Number::New(GET(Int64));
        case FieldDescriptor::CPPTYPE_UINT64:
          return Number::New(GET(UInt64));
        case FieldDescriptor::CPPTYPE_FLOAT:
          return Number::New(GET(Float));
        case FieldDescriptor::CPPTYPE_DOUBLE:
          return Number::New(GET(Double));
        case FieldDescriptor::CPPTYPE_BOOL:
          return Boolean::New(GET(Bool));
        case FieldDescriptor::CPPTYPE_ENUM:
          return String::New(GET(Enum)->name().c_str());
	}

	return Handle<Value>();  // NOTREACHED
      }
#undef GET

      Handle<Object> ToJs(const Message& instance) const {
	const Reflection* reflection = instance.GetReflection();
	const Descriptor* descriptor = instance.GetDescriptor();

	Handle<Array> properties = Array::New(descriptor->field_count());
	for (int i = 0; i < descriptor->field_count(); i++) {
          HandleScope scope;

	  const FieldDescriptor* field = descriptor->field(i);
	  bool repeated = field->is_repeated();
	  if (repeated && !reflection->FieldSize(instance, field)) continue;
	  if (!repeated && !reflection->HasField(instance, field)) continue;

	  const Type* child_type =
            (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) ?
	    schema_->GetType(field->message_type()) : NULL;

	  Handle<Value> value;
	  if (field->is_repeated()) {
	    int size = reflection->FieldSize(instance, field);
	    Handle<Array> array = Array::New(size);
	    for (int j = 0; j < size; j++) {
	      array->Set(j, ToJs(instance, reflection, field, child_type, j));
	    }
	    value = array;
	  } else {
	    value = ToJs(instance, reflection, field, child_type, -1);
	  }

	  properties->Set(i, value);
	}

        return NewObject(properties);
      }

      static Handle<Value> Parse(const Arguments& args) {
        Type* type = UnwrapThis<Type>(args);
	Buffer* buf = ObjectWrap::Unwrap<Buffer>(args[0]->ToObject());

	Message* message = type->NewMessage();
	message->ParseFromArray(buf->data(), buf->length());
	Handle<Object> result = type->ToJs(*message);
	delete message;

	return result;
      }

#define SET(TYPE, EXPR)							\
      if (repeated) reflection->Add##TYPE(instance, field, EXPR);	\
      else reflection->Set##TYPE(instance, field, EXPR)

      static void ToProto(Message* instance,
                          const FieldDescriptor* field,
                          Handle<Value> value,
                          const Type* type,
                          bool repeated) {
	HandleScope scope;

	const Reflection* reflection = instance->GetReflection();
	switch (field->cpp_type()) {
        case FieldDescriptor::CPPTYPE_MESSAGE:
          type->ToProto(repeated ?
			reflection->AddMessage(instance, field) :
			reflection->MutableMessage(instance, field),
			As<Object>(value));
          break;
        case FieldDescriptor::CPPTYPE_STRING: {
          String::AsciiValue ascii(value);
	  SET(String, string(*ascii, ascii.length()));
          break;
        }
        case FieldDescriptor::CPPTYPE_INT32:
          SET(Int32, value->NumberValue());
          break;
        case FieldDescriptor::CPPTYPE_UINT32:
          SET(UInt32, value->NumberValue());
          break;
        case FieldDescriptor::CPPTYPE_INT64:
          SET(Int64, value->NumberValue());
          break;
        case FieldDescriptor::CPPTYPE_UINT64:
          SET(UInt64, value->NumberValue());
          break;
        case FieldDescriptor::CPPTYPE_FLOAT:
          SET(Float, value->NumberValue());
          break;
        case FieldDescriptor::CPPTYPE_DOUBLE:
          SET(Double, value->NumberValue());
          break;
        case FieldDescriptor::CPPTYPE_BOOL:
          SET(Bool, value->BooleanValue());
          break;
        case FieldDescriptor::CPPTYPE_ENUM:
          SET(Enum,
	      value->IsNumber() ?
	      field->enum_type()->FindValueByNumber(value->Int32Value()) :
	      field->enum_type()->FindValueByName(*String::AsciiValue(value)));
          break;
	}
      }
#undef SET

      void ToProto(Message* instance, Handle<Object> src) const {
        Handle<Function> to_array = As<Function>(handle_->GetInternalField(3));
	Handle<Array> properties = As<Array>(to_array->Call(src, 0, NULL));
	for (int i = 0; i < descriptor_->field_count(); i++) {
	  Handle<Value> value = properties->Get(i);
	  if (value->IsUndefined()) continue;

	  const FieldDescriptor* field = descriptor_->field(i);
	  const Type* child_type =
            (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) ?
	    schema_->GetType(field->message_type()) : NULL;
	  if (field->is_repeated()) {
	    if(!value->IsArray()) {
	      ToProto(instance, field, value, child_type, true);
	    } else {
	      Handle<Array> array = As<Array>(value);
	      int length = array->Length();
	      for (int j = 0; j < length; j++) {
		ToProto(instance, field, array->Get(j), child_type, true);
	      }
	    }
	  } else {
	    ToProto(instance, field, value, child_type, false);
	  }
	}
      }

      static Handle<Value> Serialize(const Arguments& args) {
	if (!args[0]->IsObject()) {
	  return v8::ThrowException(args[0]);
	}

        Type* type = UnwrapThis<Type>(args);
	Message* message = type->NewMessage();
	type->ToProto(message, As<Object>(args[0]));
	int length = message->ByteSize();
	Buffer* buffer = Buffer::New(length);
	message->SerializeWithCachedSizesToArray((google::protobuf::uint8*)buffer->data());
	delete message;

	return buffer->handle_;
      }

      static Handle<Value> ToString(const Arguments& args) {
	return String::New(UnwrapThis<Type>(args)->descriptor_->full_name().c_str());
      }
    };

    Message* NewMessage(const Descriptor* descriptor) {
      return factory_.GetPrototype(descriptor)->New();
    }

    Type* GetType(const Descriptor* descriptor) {
      Type* result = types_[descriptor];
      if (result) return result;

      result = types_[descriptor] =
	new Type(this, descriptor, TypeTemplate->GetFunction()->NewInstance());

      // managed schema->[type] link
      Handle<Array> types = As<Array>(handle_->GetInternalField(1));
      types->Set(types->Length(), result->handle_);
      return result;
    }

    const DescriptorPool* pool_;
    map<const Descriptor*, Type*> types_;
    DynamicMessageFactory factory_;

    static Handle<Value> GetType(const Local<String> name,
				 const AccessorInfo& args) {
      Schema* schema = UnwrapThis<Schema>(args);
      const Descriptor* descriptor =
	schema->pool_->FindMessageTypeByName(*String::AsciiValue(name));

      return descriptor ?
	schema->GetType(descriptor)->Constructor() :
	Handle<Function>();
    }

    static Handle<Value> NewSchema(const Arguments& args) {
      if (!args.Length()) {
	return (new Schema(args.This(),
			   DescriptorPool::generated_pool()))->handle_;
      }

      Buffer* buf = ObjectWrap::Unwrap<Buffer>(args[0]->ToObject());

      FileDescriptorSet descriptors;
      if (!descriptors.ParseFromArray(buf->data(), buf->length())) {
	return v8::ThrowException(String::New("Malformed descriptor"));
      }

      DescriptorPool* pool = new DescriptorPool;
      for (int i = 0; i < descriptors.file_size(); i++) {
	pool->BuildFile(descriptors.file(i));
      }

      return (new Schema(args.This(), pool))->handle_;
    }
  };

 // services

  class WrappedService : public ObjectWrap {
  public:
    static Handle<Value> Invoke(const Arguments& args) {
      Service* service = UnwrapThis<WrappedService>(args)->service_;
      MethodDescriptor* method = static_cast<MethodDescriptor*>(External::Unwrap(args[0]));
      Schema* schema = ObjectWrap::Unwrap<Schema>(As<Object>(args.This()->GetInternalField(1)));

      Message* request = service->GetRequestPrototype(method).New();
      Message* response = service->GetResponsePrototype(method).New();
      schema->GetType(method->input_type())->ToProto(request, As<Object>(args[1]));
      service->CallMethod(method, NULL, request, response, NULL);
      Handle<Object> result =  schema->GetType(method->output_type())->ToJs(*response);

      delete response;
      delete request;

      return result;
    }

    WrappedService(Service* service) : service_(service) {
      Wrap(ServiceTemplate->GetFunction()->NewInstance());

      const ServiceDescriptor* descriptor = service->GetDescriptor();
      Schema* schema = new Schema(ServiceSchemaTemplate->GetFunction()->NewInstance(),
				  descriptor->file()->pool());
      handle_->SetInternalField(1, schema->handle_);

      Handle<Function> bind =
	As<Function>(Script::Compile(String::New(
            "(function(m) { var f = this; return function(arg) { return f.call(this, m, arg); }; })"))->Run());
      for (int i = 0; i < descriptor->method_count(); i++) {
	const MethodDescriptor* method = descriptor->method(i);
	Handle<Value> arg = External::Wrap(const_cast<MethodDescriptor*>(method));
	handle_->Set(String::New(method->name().c_str()),
		     bind->Call(MethodTemplate->GetFunction(), 1, &arg));
      }
    }
    
    virtual ~WrappedService() {
      delete service_;
    }
  private:
    Service* service_;
  };

  void Export(Handle<Object> target, const char* name, Service* service) {
    target->Set(String::New(name), (new WrappedService(service))->handle_);
  }

  static void Init() {
    TypeTemplate = Persistent<FunctionTemplate>::New(FunctionTemplate::New());
    TypeTemplate->SetClassName(String::New("Type"));
    // native self
    // owning schema (so GC can manage our lifecyle)
    // constructor
    // toArray
    TypeTemplate->InstanceTemplate()->SetInternalFieldCount(4);

    SchemaTemplate = Persistent<FunctionTemplate>::New(FunctionTemplate::New(Schema::NewSchema));
    SchemaTemplate->SetClassName(String::New("Schema"));
    // native self
    // array of types (so GC can manage our lifecyle)
    SchemaTemplate->InstanceTemplate()->SetInternalFieldCount(2);
    SchemaTemplate->InstanceTemplate()->SetNamedPropertyHandler(Schema::GetType);

    ServiceSchemaTemplate = Persistent<FunctionTemplate>::New(FunctionTemplate::New(Schema::NewSchema));
    ServiceSchemaTemplate->SetClassName(String::New("Schema"));
    // native self
    // array of types (so GC can manage our lifecyle)
    ServiceSchemaTemplate->InstanceTemplate()->SetInternalFieldCount(2);
    ServiceSchemaTemplate->InstanceTemplate()->SetNamedPropertyHandler(Schema::GetType);

    ServiceTemplate = Persistent<FunctionTemplate>::New(FunctionTemplate::New(Schema::NewSchema));
    ServiceTemplate->SetClassName(String::New("Service"));
    ServiceTemplate->InstanceTemplate()->SetInternalFieldCount(2);

    MethodTemplate  = Persistent<FunctionTemplate>::New(FunctionTemplate::New(WrappedService::Invoke));

    ParseTemplate = Persistent<FunctionTemplate>::New(FunctionTemplate::New(Schema::Type::Parse));
    SerializeTemplate = Persistent<FunctionTemplate>::New(FunctionTemplate::New(Schema::Type::Serialize));
  }

}  // namespace protobuf_for_node

extern "C" void __attribute__ ((constructor)) init_function_templates(void) {
  protobuf_for_node::Init();
}

extern "C" void init(Handle<Object> target) {
  target->Set(String::New("Schema"),
	      protobuf_for_node::SchemaTemplate->GetFunction());
}
