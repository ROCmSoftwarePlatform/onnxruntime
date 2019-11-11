// Copyright(C) 2019 Intel Corporation
// Licensed under the MIT License

#include "core/common/common.h"
#include "core/common/logging/logging.h"
#include "core/framework/compute_capability.h"
#include "core/framework/allocatormgr.h"
#include "core/framework/kernel_registry.h"
#include "core/framework/memcpy.h"
#include "core/graph/graph_viewer.h"
#include "core/graph/model.h"
#include "core/session/onnxruntime_cxx_api.h"
#include "migraphx_inc.h"
#include "migraphx_execution_provider.h"
#include "hip_allocator.h"
#include "gpu_data_transfer.h"
#include <fstream>

#if defined(_MSC_VER)
#pragma warning(disable : 4244 4245)
#elif __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#if defined(_MSC_VER)
#pragma warning(default : 4244 4245)
#elif __GNUC__
#pragma GCC diagnostic pop
#endif

#define MEMCPY_S(dest, src, destsz, srcsz) memcpy(dest, src, std::min(destsz, srcsz))

namespace onnxruntime {


ONNX_OPERATOR_KERNEL_EX(
    MemcpyFromHost,
    kOnnxDomain,
    1,
    kMiGraphXExecutionProvider,
    KernelDefBuilder()
        .InputMemoryType<OrtMemTypeCPUInput>(0)
        .ExecQueueId(kHipStreamCopyIn)
        .TypeConstraint("T", DataTypeImpl::AllFixedSizeTensorTypes()),
    Memcpy);

ONNX_OPERATOR_KERNEL_EX(
    MemcpyToHost,
    kOnnxDomain,
    1,
    kMiGraphXExecutionProvider,
    KernelDefBuilder()
        .OutputMemoryType<OrtMemTypeCPUOutput>(0)
        .ExecQueueId(kHipStreamCopyOut)
        .TypeConstraint("T", DataTypeImpl::AllFixedSizeTensorTypes()),
    Memcpy);

class ONNX_OPERATOR_KERNEL_CLASS_NAME(kMiGraphXExecutionProvider, kOnnxDomain, 1, MemcpyFromHost);
class ONNX_OPERATOR_KERNEL_CLASS_NAME(kMiGraphXExecutionProvider, kOnnxDomain, 1, MemcpyToHost);

static void RegisterMiGraphXKernels(KernelRegistry& kernel_registry) {
  static const BuildKernelCreateInfoFn function_table[] = {
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kMiGraphXExecutionProvider, kOnnxDomain, 1, MemcpyFromHost)>,
      BuildKernelCreateInfo<ONNX_OPERATOR_KERNEL_CLASS_NAME(kMiGraphXExecutionProvider, kOnnxDomain, 1, MemcpyToHost)>,
  };

  for (auto& function_table_entry : function_table) {
    kernel_registry.Register(function_table_entry());
  }
}

std::shared_ptr<KernelRegistry> GetMiGraphXKernelRegistry() {
  std::shared_ptr<KernelRegistry> kernel_registry = std::make_shared<KernelRegistry>();
  RegisterMiGraphXKernels(*kernel_registry);

  return kernel_registry;
}

std::shared_ptr<KernelRegistry> MiGraphXExecutionProvider::GetKernelRegistry() const {
  static std::shared_ptr<KernelRegistry> kernel_registry = onnxruntime::GetMiGraphXKernelRegistry();
  return kernel_registry;
}

constexpr const char* MIGRAPHX = "MiGraphX";

MiGraphXExecutionProvider::MiGraphXExecutionProvider(const MiGraphXExecutionProviderInfo& info)
    : IExecutionProvider{onnxruntime::kMiGraphXExecutionProvider} {

  // Set GPU device to be used
  hipSetDevice(info.device_id);

  DeviceAllocatorRegistrationInfo default_memory_info(
      {OrtMemTypeDefault, [](int id) { return onnxruntime::make_unique<HIPAllocator>(id, TRT); }, std::numeric_limits<size_t>::max()});
  allocator_ = CreateAllocator(default_memory_info, device_id_);
  InsertAllocator(allocator_);


  DeviceAllocatorRegistrationInfo pinned_memory_info(
      {OrtMemTypeCPUOutput, [](int) { return onnxruntime::make_unique<HIPPinnedAllocator>(0, TRT_PINNED); }, std::numeric_limits<size_t>::max()});
  InsertAllocator(CreateAllocator(pinned_memory_info, device_id_));


  // create the target based on the device_id
  hipDeviceProp_t prop;
  hipGetDeviceProperties(&prop, device_id_);

  if (info.target_device == "cpu")
  {
    migraphx::target t = migraphx::cpu::target{};
    t_ = t;
  }
  else if (info.target_device == "gpu")
  {
    migraphx::target t = migraphx::gpu::target{};
    t_ = t;
  }
  else
  {
    LOGS_DEFAULT(FATAL) << "Device " << info.target_device << " are not supported";    
  }
}

AllocatorPtr MiGraphXExecutionProvider::GetAllocator(int id, OrtMemType mem_type) const {
  if (mem_type == OrtMemTypeDefault) {
    return allocator_;
  } else {
    return IExecutionProvider::GetAllocator(id, mem_type);
  }
}

std::unique_ptr<onnxruntime::IDataTransfer> MiGraphXExecutionProvider::GetDataTransfer() const {
  return onnxruntime::make_unique<onnxruntime::GPUDataTransfer>();
}

static bool IsTypeSupported(const NodeArg* node_arg) {
  const auto* type_proto = node_arg->TypeAsProto();
  if (!type_proto) {
    return false;
  }

  switch (type_proto->tensor_type().elem_type()) {
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_FLOAT16:
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_FLOAT:
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_DOUBLE:
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT8:
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT16:
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT32:
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT64:
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT8:
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT16:
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT32:
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT64:
      return true;
    default:
      return false;
  }
}

static bool get_migraphx_type(ONNXTensorElementDataType type, 
                              migraphx::shape::type_t &mgx_type)
{
  mgx_type = migraphx::shape::float_type;
  switch(type) {
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_FLOAT16:
      mgx_type = migraphx::shape::half_type;
      break;
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_FLOAT:
      mgx_type = migraphx::shape::float_type;
      break;
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_DOUBLE:
      mgx_type = migraphx::shape::double_type;
      break;
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT8:
      mgx_type = migraphx::shape::int8_type;
      break;
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT16:
      mgx_type = migraphx::shape::int16_type;
      break;
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT32:
      mgx_type = migraphx::shape::int32_type;
      break;
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_INT64:
      mgx_type = migraphx::shape::int64_type;
      break;
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT8:
      mgx_type = migraphx::shape::uint8_type;
      break;
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT16:
      mgx_type = migraphx::shape::uint16_type;
      break;
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT32:
      mgx_type = migraphx::shape::uint32_type;
      break;
    case ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_UINT64:
      mgx_type = migraphx::shape::uint64_type;
      break;
    default:
      LOGS_DEFAULT(WARNING) << "MiGraphx: unsupported data type " << type << ", fallback to CPU";
      LOGS_DEFAULT(WARNING) << "implementation" << std::endl;
      return false;
  }

  return true;
}

static bool IsNodeSupported(const std::set<std::string>& op_set,
                            const onnxruntime::GraphViewer& graph_viewer,
                            const NodeIndex node_idx) {
  const auto& node = graph_viewer.GetNode(node_idx);
  const auto& optype = node->OpType();
  // const auto& domain = node->Domain();

  // 1. Check input and output data types are supported.
  // 2. Check Op is supported

  //Check 1
  bool are_types_supported = true;

  node->ForEachDef([&are_types_supported](const onnxruntime::NodeArg& node_arg, bool /*is_input*/) {
    are_types_supported &= IsTypeSupported(&node_arg);
  });

  if (!are_types_supported) {
    return false;
  }

  // Check 2
  if (op_set.count(optype) > 0) {
    return true;
  } else {
    return false;
  }
}

static void AppendNodesToSubGraph(const std::vector<NodeIndex>& nodes,
                                    const std::vector<std::string>& inputs,
                                    const std::vector<std::string>& outputs,
                                    std::vector<std::unique_ptr<ComputeCapability>>& result) {
  static size_t op_counter = 0;

  auto meta_def = onnxruntime::make_unique<IndexedSubGraph::MetaDef>();
  meta_def->name = "MIGraphX_" + std::to_string(++op_counter);
  meta_def->domain = kMiGraphXDomain;
  meta_def->since_version = 1;
  meta_def->status = ONNX_NAMESPACE::EXPERIMENTAL;
  meta_def->inputs = inputs;
  meta_def->outputs = outputs;

  std::unique_ptr<IndexedSubGraph> sub_graph = onnxruntime::make_unique<IndexedSubGraph>();
  sub_graph->nodes = nodes;
  sub_graph->SetMetaDef(meta_def);
  result.push_back(onnxruntime::make_unique<ComputeCapability>(std::move(sub_graph)));
}

// static int GetOnnxOpSet(const GraphViewer& graph_viewer) {
//   const auto& dm_to_ver = graph_viewer.DomainToVersionMap();
//   return dm_to_ver.at(kOnnxDomain);
// }

static std::set<std::string> GetMiGraphXSupportedOps() {
  std::set<std::string> mgx_supported_ops = migraphx::get_supported_ops();
  return mgx_supported_ops;
}

static std::vector<NodeIndex>
GetUnsupportedNodeIndices(const GraphViewer& graph_viewer, /*out*/ std::unordered_set<std::string>& mgx_required_initializers) {
  const auto mgx_supported_ops = GetMiGraphXSupportedOps();

  std::vector<NodeIndex> unsupported_nodes_idx;

  for (const auto& node_idx : graph_viewer.GetNodesInTopologicalOrder()) {
    if (IsNodeSupported(mgx_supported_ops, graph_viewer, node_idx)) {
      // Collect inputs that are initializers
      graph_viewer.GetNode(node_idx)->ForEachDef([&mgx_required_initializers, &graph_viewer](const onnxruntime::NodeArg& node_arg, bool is_input) {
              if(is_input && graph_viewer.GetAllInitializedTensors().count(node_arg.Name())) {
                mgx_required_initializers.insert(node_arg.Name());
              } }, true);
    } else {
      unsupported_nodes_idx.push_back(node_idx);
    }
  }

  return unsupported_nodes_idx;
}

// Returns a vector clusters(or node_idx). For each unsupported node, the graph
// is split into 3 parts. supported_cluster + (UNsupported_node + rest_of_the_graph). 
// This functions returns vector of all supported_subgraphx by amdmigraphx
static std::vector<std::vector<NodeIndex>>
GetPartitionedSubgraphs(const std::vector<NodeIndex>& topological_order, const std::vector<NodeIndex>& unsupported_nodes) {
  std::vector<std::vector<NodeIndex>> mgx_subgraphx;

  auto prev = topological_order.begin();

  for (const auto& unsup_node : unsupported_nodes) {
    auto it = std::find(prev, topological_order.end(), unsup_node);
    // Create a cluster vector[supported_node_idx, unsupported_node_idx) 
    // and append it to return list.
    std::vector<NodeIndex> this_subgraph{prev, it};
    if (!this_subgraph.empty()) {
      mgx_subgraphx.push_back(std::move(this_subgraph));
    }
    // Point prev to node idx past this unsuported node.
    prev = ++it;
  }

  // Tail
  std::vector<NodeIndex> this_subgraph{prev, topological_order.end()};
  if (!this_subgraph.empty()) {
    mgx_subgraphx.push_back(std::move(this_subgraph));
  }

  return mgx_subgraphx;
}

static void GetInputsOutputsOfSubgraph(const GraphViewer& graph_viewer,
                                      const std::vector<NodeIndex>& nodes,
                                      const std::unordered_set<std::string>& mgx_required_initializers,
                                      /*output*/ std::vector<std::string>& nodes_inputs,
                                      /*output*/ std::vector<std::string>& nodes_outputs) {
  std::unordered_set<std::string> input_args;
  std::vector<std::string> ordered_input_args;
  std::unordered_set<std::string> output_args;
  std::unordered_set<std::string> external_output_args;

  for (const auto& node_idx : nodes) {
    const auto& node = graph_viewer.GetNode(node_idx);

    // Collect all inputs and outputs
    node->ForEachDef(
        [&input_args, &ordered_input_args, &output_args](const NodeArg& node_arg, bool is_input) {
          if (is_input) {
            if (!input_args.count(node_arg.Name())) {
              ordered_input_args.push_back(node_arg.Name());
            }
            input_args.insert(node_arg.Name());
          } else {
            output_args.insert(node_arg.Name());
          }
        },
        true);

    // Check if output of this node is used by nodes outside 
    // subgraph. If yes add this to cluster outputs
    for (auto it = node->OutputNodesBegin(); it != node->OutputNodesEnd(); ++it) {
      const auto& ext_node = graph_viewer.GetNode((*it).Index());

      if (std::find(nodes.begin(), nodes.end(), ext_node->Index()) == nodes.end()) {
        // Node is external to subgraph. Search through its 
        // inputs to find the output that is generated by subgraph.
        std::set<std::string> ext_node_inputs;
        ext_node->ForEachDef(
            [&ext_node_inputs](const onnxruntime::NodeArg& arg, bool is_input) {
              if (is_input) {
                ext_node_inputs.insert(arg.Name());
              }
            },
            true);

        for (const auto& out_def : node->OutputDefs()) {
          if (ext_node_inputs.find(out_def->Name()) != ext_node_inputs.end()) {
            external_output_args.insert(out_def->Name());
          }
        }
      }
    }
  }

  //Extract initializers used by subgraph.
  std::unordered_set<std::string> original_graph_inputs;
  for (const auto& node_arg : graph_viewer.GetInputsIncludingInitializers()) {
    original_graph_inputs.insert(node_arg->Name());
  }

  const auto& initializers = graph_viewer.GetAllInitializedTensors();
  std::vector<std::string> const_inputs;
  for (const auto& in_arg : ordered_input_args) {
    if ((initializers.count(in_arg) && !original_graph_inputs.count(in_arg)) ||
        mgx_required_initializers.count(in_arg)) {
      const_inputs.push_back(in_arg);
    }
  }

  for (const auto& in_arg : ordered_input_args) {
    if (!output_args.count(in_arg) &&
        !((initializers.count(in_arg) && !original_graph_inputs.count(in_arg)) ||
        mgx_required_initializers.count(in_arg))) {
      nodes_inputs.push_back(in_arg);
    }
  }

  for (const auto& in_arg : const_inputs) {
    nodes_inputs.push_back(in_arg);
  }

  std::copy(external_output_args.begin(), external_output_args.end(), std::back_inserter(nodes_outputs));
  for (const auto& node_arg : graph_viewer.GetOutputs()) {
    const auto& name = node_arg->Name();
    if (output_args.count(name) && !external_output_args.count(name)) {
      nodes_outputs.push_back(name);
    }
  }
}

std::vector<std::unique_ptr<ComputeCapability>>
MiGraphXExecutionProvider::GetCapability(const onnxruntime::GraphViewer& graph_viewer,
                                       const std::vector<const KernelRegistry*>& /*kernel_registries*/) const {

  std::vector<std::unique_ptr<ComputeCapability>> result;
  if (graph_viewer.IsSubgraph()) {
    return result;
  }

  for (const auto& tensor : graph_viewer.GetAllInitializedTensors()) {
    if (tensor.second->has_data_location() && tensor.second->data_location() == ONNX_NAMESPACE::TensorProto_DataLocation_EXTERNAL) {
      LOGS_DEFAULT(WARNING) << "MIGraphX: Initializers with external data location are not currently supported";
      return result;
    }
  }

  // Construct modelproto from graph
  onnxruntime::Model model(graph_viewer.Name(), true, ModelMetaData(), IOnnxRuntimeOpSchemaRegistryList(), graph_viewer.DomainToVersionMap());
  onnxruntime::Graph& graph_build = model.MainGraph();
  for (const auto& node : graph_viewer.Nodes()) {
    std::vector<onnxruntime::NodeArg*> inputs, outputs;
    for (auto input : node.InputDefs()) {
      auto& n_input = graph_build.GetOrCreateNodeArg(input->Name(), input->TypeAsProto());
      inputs.push_back(&n_input);
    }
    for (auto output : node.OutputDefs()) {
      auto& n_output = graph_build.GetOrCreateNodeArg(output->Name(), output->TypeAsProto());
      outputs.push_back(&n_output);
    }
    graph_build.AddNode(node.Name(), node.OpType(), node.Description(), inputs, outputs, &node.GetAttributes(), node.Domain());
  }

  auto status = graph_build.Resolve();

  //Add initializer to graph
  const auto& init_tensors = graph_viewer.GetAllInitializedTensors();
  for (const auto& tensor : init_tensors) {
    graph_build.AddInitializedTensor(*(tensor.second));
  }

  ORT_ENFORCE(status.IsOK(), status);
  ONNX_NAMESPACE::ModelProto model_proto = model.ToProto();
  model_proto.set_ir_version(ONNX_NAMESPACE::Version::IR_VERSION);

  // migraphx now can only support on output. if there are multiple
  // outputs, we cannot support this model
  std::size_t num_outputs = model_proto.graph().output_size();
  if (num_outputs > 1)
  {
      LOGS_DEFAULT(WARNING) << "MIGraphX can support only one output, but input model";
      LOGS_DEFAULT(WARNING) << "has " << num_outputs << " outputs, so fall back to";
      LOGS_DEFAULT(WARNING) << "default CPU implementation!";

      return result;
  }

  // migraphx now cannot support inputs with dynamic shape
  std::size_t num_inputs = model_proto.graph().input_size();
  std::cout << "input_num = " << num_inputs << std::endl;
  for (std::size_t in_index = 0; in_index < num_inputs; ++in_index)
  {
    auto in_node = model_proto.graph().input(in_index);
    const NodeArg* node_arg = graph_viewer.GetNodeArg(in_node.name());
    if (node_arg == nullptr) continue;
    auto&& type_as_proto = node_arg->TypeAsProto();
    auto& dims = type_as_proto->tensor_type().shape().dim();
    for (auto&& d : dims)
    {
      if (not d.has_dim_value())
      {
        LOGS_DEFAULT(WARNING) << "MiGraphX, model input " << in_node.name(); 
        LOGS_DEFAULT(WARNING) << "is dynamic shape, not supported. Fallback";
        LOGS_DEFAULT(WARNING) << "to default CPU execution!" << std::endl;

        return result;
      }
    }
  }

  std::string string_buf;
  model_proto.SerializeToString(&string_buf);

  // may not be needed since it can return false in many scenarios
  std::vector<std::string> unsupported_nodes_temp;
  migraphx::program prog = migraphx::parse_model(string_buf, unsupported_nodes_temp);
  if (prog.size() == 0)
  {
    return result;
  }

  if (unsupported_nodes_temp.size())
  {
    std::cout << "Unsupported nodes from onnxruntime check====================: " << std::endl;
    for (auto& node_name : unsupported_nodes_temp)
    {
      std::cout << node_name << std::endl;
    }
    std::cout << "End of unsupported nodes from onnxruntime check============" << std::endl;
  }

  // This is a list of initializers that migraphx considers as constants. 
  // Example weights, reshape shape etc.
  std::unordered_set<std::string> mgx_required_initializers;
  const auto unsupported_nodes = GetUnsupportedNodeIndices(graph_viewer, mgx_required_initializers);

  //If all ops are supported, no partitioning is required. Short-circuit and avoid splitting.
  if (unsupported_nodes.empty()) {
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;

    //Fill inputs with names
    std::for_each(graph_viewer.GetInputs().begin(), graph_viewer.GetInputs().end(),
                  [&inputs](const NodeArg* node_arg) { inputs.push_back(node_arg->Name()); });

    // In scenarios, when there are no inputs or all inputs being initializers,
    // ConstantFolding optimization in onnxruntime pre-computes the value.
    if (inputs.empty()) {
      return result;
    }

    // Initializers need to be part of meta_def->inputs
    std::for_each(mgx_required_initializers.begin(), mgx_required_initializers.end(),
                  [&inputs](const std::string& initializer) { inputs.push_back(initializer); });

    // Fill outputs with names
    std::for_each(graph_viewer.GetOutputs().begin(), graph_viewer.GetOutputs().end(),
                  [&outputs](const NodeArg* node_arg) { outputs.push_back(node_arg->Name()); });

    // Create and add this graph to result.
    AppendNodesToSubGraph(graph_viewer.GetNodesInTopologicalOrder(), inputs, outputs, result);

  } else {  // unsupported_nodes_idx.empty()
    const auto mgx_clusters = GetPartitionedSubgraphs(graph_viewer.GetNodesInTopologicalOrder(), unsupported_nodes);

    for (const auto& this_cluster : mgx_clusters) {
      std::vector<std::string> cluster_inputs, cluster_outputs;
      GetInputsOutputsOfSubgraph(graph_viewer, this_cluster, mgx_required_initializers, cluster_inputs, cluster_outputs);

      if (!cluster_inputs.empty()) {
        AppendNodesToSubGraph(this_cluster, cluster_inputs, cluster_outputs, result);
      }
    }
  }

  return result;
}

static ONNX_NAMESPACE::ModelProto GetModelProtoFromFusedNode(const onnxruntime::Node* fused_node) {
  const auto* node_function = fused_node->GetFunctionBody();

  ORT_ENFORCE(node_function != nullptr, "Could not extract function body for node: ", fused_node->Name());

  const Graph& node_subgraph = node_function->Body();
  onnxruntime::Model model{node_subgraph.Name(), true};

  ONNX_NAMESPACE::ModelProto model_proto = model.ToProto();
  //model_proto.set_ir_version(ONNX_NAMESPACE::Version::IR_VERSION);

  *(model_proto.mutable_graph()) = node_subgraph.ToGraphProto();

  auto opset = model_proto.add_opset_import();
  opset->set_domain(kOnnxDomain);
  opset->set_version(node_subgraph.DomainToVersionMap().at(kOnnxDomain));

  return model_proto;
}

Status MiGraphXExecutionProvider::Compile(const std::vector<onnxruntime::Node*>& fused_nodes,
                                        std::vector<NodeComputeInfo>& node_compute_funcs) {
  for (const auto& fused_node : fused_nodes) {
    // map parameter input name to index
    std::unordered_map<std::string, std::size_t> input_name_index;
    const auto& input_defs = fused_node->InputDefs();
    input_name_index.reserve(input_defs.size());
    for (std::size_t i = 0; i < input_defs.size(); ++i) {
      input_name_index[input_defs[i]->Name()] = i;
    }

    // record name of each output
    std::unordered_map<std::string, std::size_t> output_name_index;
    const auto& output_defs = fused_node->OutputDefs();
    output_name_index.reserve(output_defs.size());
    for (std::size_t i = 0; i < output_defs.size(); ++i) {
      output_name_index[output_defs[i]->Name()] = i;
    }

    // reconstruct the subgraph proto from fused nodes
    onnx::ModelProto model_proto = GetModelProtoFromFusedNode(fused_node);
    std::string string_buf;
    model_proto.SerializeToString(&string_buf);

    // Debugging purpose, write the model out as a binary file
    // std::ofstream ort_tmp_file("ort_tmp.bin", std::ofstream::binary);
    // ort_tmp_file.write(string_buf.c_str(), string_buf.size());
    // ort_tmp_file.close();

    // by parsing the model_proto, create a program corresponding to
    // the input fused_node
    std::vector<std::string> unsupported_nodes;
    migraphx::program prog = migraphx::parse_model(string_buf, unsupported_nodes);
    std::cout << "prog = " << std::endl;
    std::cout << prog << std::endl;

    // compile the program
    prog.compile(t_);
    map_progs_[fused_node->Name()] = prog;

    std::unordered_map<std::size_t, std::size_t> input_index_map;
    std::unordered_map<std::size_t, std::size_t> output_index_map;
    std::unordered_map<std::string, migraphx::shape> param_shapes = prog.get_parameter_shapes();
    std::size_t param_index = 0;
    for (auto &&x : param_shapes)
    {
      // process the input
      auto iit = input_name_index.find(x.first);
      if (iit != input_name_index.end())
      {
        input_index_map[param_index] = iit->second;
      }

      // process the output
      auto oit = output_name_index.find(x.first);
      if (oit != output_name_index.end())
      {
        output_index_map[param_index] = oit->second;
      }
      ++param_index;
    }

    // process the scratch memory
    auto sit = param_shapes.find("scratch");
    if (sit != param_shapes.end())
    {
      std::cout << "Scartch allocated, shape = " << sit->second << std::endl;
      map_scratches_[fused_node->Name()] = t_.copy_to(migraphx::generate_argument(sit->second));
    }

    // hack, manually add the output index to the output_index
    output_index_map[99999] = output_name_index.begin()->second;

    map_input_index_[fused_node->Name()] = input_index_map;
    map_output_index_[fused_node->Name()] = output_index_map;

    NodeComputeInfo compute_info;
    compute_info.create_state_func = [=](ComputeContext* context, FunctionState* state) {
      std::unique_ptr<MiGraphXFuncState> p = onnxruntime::make_unique<MiGraphXFuncState>();
      *p = {context->allocate_func, context->release_func, context->allocator_handle, map_progs_[context->node_name], t_,
            map_scratches_[context->node_name], map_input_index_[context->node_name], map_output_index_[context->node_name], &mgx_mu_};
      *state = p.release();
      return 0;
    };

    compute_info.release_state_func = [](FunctionState state) {
      if (state)
        delete static_cast<MiGraphXFuncState*>(state);
    };

    compute_info.compute_func = [](FunctionState state, const OrtCustomOpApi* api, OrtKernelContext* context) {
      Ort::CustomOpApi ort{*api};
      MiGraphXFuncState* mgx_state = reinterpret_cast<MiGraphXFuncState*>(state);
      std::unordered_map<std::size_t, std::size_t>& map_input_index = mgx_state->input_indexes;
      std::unordered_map<std::size_t, std::size_t>& map_output_index = mgx_state->output_indexes;
      migraphx::target t = mgx_state->t;
      migraphx::program& prog = mgx_state->prog;
      // migraphx::argument& scratch = mgx_state->scratch;

      std::unordered_map<std::string, migraphx::shape> param_shapes = prog.get_parameter_shapes();
      migraphx::program::parameter_map m;
      m.reserve(param_shapes.size());

      std::size_t param_index = 0;
      for (auto&& x : param_shapes)
      {
        if (map_input_index.count(param_index) > 0)
        {
          const OrtValue* input_tensor = ort.KernelContext_GetInput(context, map_input_index[param_index]);
          auto tensor_info = ort.GetTensorTypeAndShape(input_tensor);
          const auto& tensor_shape = ort.GetTensorShape(tensor_info);
          auto tensor_type = ort.GetTensorElementType(tensor_info);
          ort.ReleaseTensorTypeAndShapeInfo(tensor_info);

          migraphx::shape::type_t mgx_type;
          get_migraphx_type(tensor_type, mgx_type);
          auto mgx_s = x.second;

          if (mgx_type != mgx_s.type())
          {
            MIGRAPHX_THROW("MIGraphX: param type mismatch");
          }
          m[x.first] = migraphx::argument(x.second, const_cast<void*>(ort.GetTensorData<void>(input_tensor)));
        }
        param_index++;
      }

      // process output, there is only one output here
      for (auto&& x : param_shapes)
      {
        if (x.first == std::string("output"))
        {
          migraphx::shape res_shape = param_shapes["output"];
          std::size_t output_index = map_output_index.begin()->second;
          std::vector<int64_t> ort_shape{res_shape.lens().begin(), res_shape.lens().end()};
          OrtValue* output_tensor = ort.KernelContext_GetOutput(context, output_index, ort_shape.data(), ort_shape.size());
          void* output_data = ort.GetTensorMutableData<void>(output_tensor);
          m["output"] = migraphx::argument(res_shape, output_data);
        }
      }

      // scratch memory
      for (auto&& x : param_shapes)
      {
        if (m.count(x.first) == 0)
        {
          m[x.first] = t.copy_to(migraphx::generate_argument(x.second));
        }
      }

      {
        // lock to avoid race condition
        std::lock_guard<OrtMutex> lock(*(mgx_state->mgx_mu_ptr));
        prog.eval(m);
      }

      return Status::OK();
    };

    node_compute_funcs.push_back(compute_info);
  }

  return Status::OK();
}

}  // namespace onnxruntime
