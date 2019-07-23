#include "operators.h"
#include <tvm/relay/attrs/nn.h>
#include <tvm/relay/attrs/transform.h>
#include "compiler.h"
#include "fusion_pass.h" // tvm_sym

#include <torch/csrc/autograd/record_function.h>
#include <torch/csrc/jit/custom_operator.h>
#include <torch/csrc/jit/operator_options.h>
#include <torch/csrc/jit/passes/utils/subgraph_utils.h>

using namespace torch::jit;

std::unordered_map<std::string, TVMScheduleFunctor>& getTVMScheduleMap() {
  static std::unordered_map<std::string, TVMScheduleFunctor> map;
  return map;
}

std::unordered_map<Symbol, TVMOpFunctor>& getTVMOperatorMap() {
  static std::unordered_map<Symbol, TVMOpFunctor> map;
  return map;
}

// These "wrapper" graphs are used to store the subgraphs
// that will be compiled during execution.
static std::list<Graph> wrapper_graphs;
RegisterTVMOperator::RegisterTVMOperator(std::vector<TVMOpMap> ops) {
  for (const auto& op : ops) {
    getTVMOperatorMap()[op.sym] = op.fn;

    if (op.name != "") {
      auto torch_ops = getAllOperatorsFor(op.sym);

      for (const auto& torch_op : torch_ops) {
        auto schema = torch_op->schema();

        wrapper_graphs.emplace_back();
        auto& wrapper_graph = wrapper_graphs.back();
        std::vector<Value*> torch_inputs;
        for (const auto& inp : schema.arguments()) {
          torch_inputs.emplace_back(wrapper_graph.addInput());
        }
        Node* node =
            wrapper_graph.create(op.sym, torch_inputs, schema.returns().size());
        wrapper_graph.appendNode(node);
        wrapper_graph.registerOutput(node->output());

        node = SubgraphUtils::createSingletonSubgraph(node, getTVMSymbol());
        auto cc = std::make_shared<TVMCompiler>(node);

        // NB: We assume all relay ops are pure
        auto options = c10::OperatorOptions();
        options.setAliasAnalysis(AliasAnalysisKind::PURE);
	// TODO: Pass in operator options somehow
        auto torch_operator = Operator(
            FunctionSchema(
                "tvm::" + op.name,
                "",
                schema.arguments(),
                schema.returns(),
                false,
                false),
	      [cc](Stack& stack) {
		RECORD_FUNCTION("TVM", std::vector<c10::IValue>());
		cc->run(stack);
		return 0;
            });
        RegisterOperators torch_register_ops(
            std::vector<Operator>{torch_operator});
      }
    }
  }
}

// This must be done lazily to prevent SIOF
void registerSchedule(std::string name) {
  TORCH_INTERNAL_ASSERT(getTVMScheduleMap().find(name) != getTVMScheduleMap().end());
  TVMScheduleFunctor sched_f = getTVMScheduleMap()[name];
  auto reg = tvm::runtime::Registry::Get("relay.op._Register");
  TORCH_INTERNAL_ASSERT(reg);
  auto sched = sched_f();
  // Relay does not provide a good API for querying the status of schedules
  if (sched) {
    (*reg)(name, "FTVMSchedule", *sched, 10);
    getTVMScheduleMap()[name] = []() { return nullptr; };
  }
}

bool isConstant(tvm::relay::Expr e) {
  auto c = e.as<tvm::relay::ConstantNode>();
  return !!c;
}

template <typename T>
T relayToConstant(tvm::relay::Expr e) {
  auto c = e.as<tvm::relay::ConstantNode>();
  TORCH_INTERNAL_ASSERT(c);
  TORCH_INTERNAL_ASSERT(c->is_scalar());
  return static_cast<T*>(c->data->data)[0];
}

bool relayIsNone(tvm::relay::Expr e) {
  if (!isConstant(e)) {
    return false;
  }
  auto c = e.as<tvm::relay::ConstantNode>();
  if (!c->is_scalar()) {
    return false;
  }
  auto val = static_cast<uint64_t*>(c->data->data)[0];
  return val == getNoneSentinel();
}

uint64_t getNoneSentinel() {
  return 0xe4fa3adecabcf036;
}

template <typename T>
tvm::Array<T> relayToArray(tvm::relay::Expr e) {
  auto t = e.as<tvm::relay::TupleNode>();
  tvm::Array<T> elems;
  for (auto c : t->fields) {
    int elem = relayToConstant<int>(c);
    elems.push_back(elem);
  }
  return elems;
}

RegisterTVMOperatorSchedule::RegisterTVMOperatorSchedule(
    std::vector<std::pair<std::string, TVMScheduleFunctor>> scheds) {
  for (const auto& pair : scheds) {
    auto name = std::get<0>(pair);
    auto sched_f = std::get<1>(pair);
    getTVMScheduleMap()[name] = sched_f;
  }
}

RegisterTVMOperator reg({
    {Symbol::fromQualString("aten::add"),
     [](Node* node, tvm::Array<tvm::relay::Expr> inputs) {
       auto op = tvm::relay::Op::Get("add");
       TORCH_INTERNAL_ASSERT(inputs.size() == 3);
       tvm::Array<tvm::relay::Expr> add_inputs = {inputs[0], inputs[1]};
       // Handle pytorch's value argument in add
       auto value = inputs[2].as<tvm::relay::ConstantNode>();
       TORCH_INTERNAL_ASSERT(
           value->is_scalar() &&
           reinterpret_cast<int*>(value->data->data)[0] == 1);
       auto out = tvm::relay::CallNode::make(op, add_inputs, tvm::Attrs(), {});
       return out;
     }},
    {Symbol::fromQualString("aten::add_"),
     [](Node* node, tvm::Array<tvm::relay::Expr> inputs) {
       auto op = tvm::relay::Op::Get("add");
       TORCH_INTERNAL_ASSERT(inputs.size() == 3);
       tvm::Array<tvm::relay::Expr> add_inputs = {inputs[0], inputs[1]};
       // Handle pytorch's value argument in add
       auto value = inputs[2].as<tvm::relay::ConstantNode>();
       TORCH_INTERNAL_ASSERT(
           value->is_scalar() &&
           reinterpret_cast<int*>(value->data->data)[0] == 1);
       auto out = tvm::relay::CallNode::make(op, add_inputs, tvm::Attrs(), {});
       return out;
     }},
    {Symbol::fromQualString("aten::_convolution"),
     [](Node* node, tvm::Array<tvm::relay::Expr> inputs) {
       bool is_transpose = relayToConstant<bool>(inputs[6]);
       // check the operator to emit base on is_transpose
       auto op = tvm::relay::Op::Get("nn.conv2d");
       if (is_transpose) {
         op = tvm::relay::Op::Get("nn.conv2d_transpose");
       }

       // input and filter
       tvm::Array<tvm::relay::Expr> new_inputs = {
           inputs[0],
           inputs[1],
       };

       auto conv_attrs = tvm::make_node<tvm::relay::Conv2DAttrs>();
       conv_attrs->groups = relayToConstant<int>(inputs[8]);
       conv_attrs->data_layout = "NCHW";
       conv_attrs->kernel_layout = "OIHW";

       conv_attrs->kernel_size =
           tvm::NullValue<tvm::Array<tvm::relay::IndexExpr>>();
       // If the input was a complete tensor type than we have information to
       // populate the kernel
       if (const tvm::relay::VarNode* var =
               inputs[1].as<tvm::relay::VarNode>()) {
         auto* w_t = var->type_annotation.as<tvm::relay::TensorTypeNode>();
         TORCH_INTERNAL_ASSERT(w_t);
         auto shape = w_t->shape;
         tvm::Array<tvm::relay::IndexExpr> w_sizes = {shape[2], shape[3]};
         conv_attrs->kernel_size = w_sizes;
       }

       conv_attrs->strides = relayToArray<tvm::relay::IndexExpr>(inputs[3]);
       conv_attrs->padding = relayToArray<tvm::relay::IndexExpr>(inputs[4]);
       conv_attrs->dilation = relayToArray<tvm::relay::IndexExpr>(inputs[5]);

       auto out = tvm::relay::CallNode::make(
           op, new_inputs, tvm::Attrs(conv_attrs), {});

       // Check if bias node is a var or constant (denoting a None currently),
       // if bias is present, emit an additional bias_add node.
       // TODO: better check when relay has None type
       auto bias_is_none = inputs[2].as<tvm::relay::ConstantNode>();
       if (!bias_is_none) {
         auto bias_add_op = tvm::relay::Op::Get("nn.bias_add");
         auto bias_add_attrs = tvm::make_node<tvm::relay::BiasAddAttrs>();
         bias_add_attrs->axis = 1;
         return tvm::relay::CallNode::make(
             bias_add_op, {out, inputs[2]}, tvm::Attrs(bias_add_attrs), {});
       }
       return out;
     }},
    {Symbol::fromQualString("aten::batch_norm"),
     [](Node* node, tvm::Array<tvm::relay::Expr> inputs) -> tvm::relay::Expr {
       auto op = tvm::relay::Op::Get("nn.batch_norm");
       TORCH_CHECK(
           inputs.size() == 9,
           "batch_norm received ",
           inputs.size(),
           " inputs");
       TORCH_CHECK(
           relayToConstant<bool>(inputs[5]) == false,
           "batch_norm is in training mode");
       auto attrs = tvm::make_node<tvm::relay::BatchNormAttrs>();
       auto eps = relayToConstant<float>(inputs[7]);
       attrs->epsilon = eps;
       attrs->axis = 1;
       attrs->scale = false;
       attrs->center = false;

       TVMContext ctx_;
       ctx_.device_type = kDLCPU;
       ctx_.device_id = 0;
       auto x = tvm::runtime::NDArray::Empty(
           {}, tvm::runtime::String2TVMType("float32"), ctx_);
       // Make this large to induce noticeable errors
       reinterpret_cast<float*>(x->data)[0] = 1337e10;
       tvm::relay::Expr v = tvm::relay::ConstantNode::make(x);

       auto& broadcast = tvm::relay::Op::Get("broadcast_to_like");
       tvm::relay::Expr weight = tvm::relay::CallNode::make(
           broadcast, {v, inputs[3]}, tvm::Attrs(), {});
       tvm::relay::Expr bias = tvm::relay::CallNode::make(
           broadcast, {v, inputs[3]}, tvm::Attrs(), {});

       // TODO check if pytorch semantics allow these to be broadcast
       if (!relayIsNone(inputs[1])) {
         attrs->scale = true;
         weight = inputs[1];
       }
       if (!relayIsNone(inputs[2])) {
         attrs->center = true;
         bias = inputs[2];
       }

       tvm::Array<tvm::relay::Expr> bn_inputs = {
           inputs[0],
           weight,
           bias,
           inputs[3],
           inputs[4],
       };

       auto out =
           tvm::relay::CallNode::make(op, bn_inputs, tvm::Attrs(attrs), {});
       TORCH_INTERNAL_ASSERT(node->outputs().size() == 1);
       if (node->outputs().size() == 2) {
         return out;
       }
       auto n = tvm::make_node<tvm::relay::TupleGetItemNode>();
       n->tuple = std::move(out);
       n->index = 0;
       return tvm::relay::TupleGetItem(n);
     }},
    {Symbol::fromQualString("aten::relu_"),
     [](Node* node, tvm::Array<tvm::relay::Expr> inputs) {
       auto op = tvm::relay::Op::Get("nn.relu");
       auto out = tvm::relay::CallNode::make(op, inputs, tvm::Attrs(), {});
       return out;
     }},
    {Symbol::fromQualString("aten::relu"),
     [](Node* node, tvm::Array<tvm::relay::Expr> inputs) {
       auto op = tvm::relay::Op::Get("nn.relu");
       auto out = tvm::relay::CallNode::make(op, inputs, tvm::Attrs(), {});
       return out;
     },
     "relu"},
    {Symbol::fromQualString("aten::threshold_"),
     [](Node* node, tvm::Array<tvm::relay::Expr> inputs) {
       TORCH_CHECK(!relayIsNone(inputs[0]));
       TORCH_CHECK(!relayIsNone(inputs[1]));
       TORCH_CHECK(!relayIsNone(inputs[2]));
       auto d = relayToConstant<float>(inputs[1]);
       TORCH_CHECK(
           d < 1e-7, "aten::threshold_ only supported for threshold 0, got", d);
       TORCH_CHECK(
           d > -1e-7,
           "aten::threshold_ only supported for threshold 0, got",
           d);
       d = relayToConstant<float>(inputs[2]);
       TORCH_CHECK(
           d < 1e-7, "aten::threshold_ only supported for value 0, got", d);
       TORCH_CHECK(
           d > -1e-7, "aten::threshold_ only supported for value 0, got", d);
       auto op = tvm::relay::Op::Get("nn.relu");
       auto out = tvm::relay::CallNode::make(op, {inputs[0]}, tvm::Attrs(), {});
       return out;
     }},
    {Symbol::fromQualString("aten::mul"),
     [](Node* node, tvm::Array<tvm::relay::Expr> inputs) {
       auto op = tvm::relay::Op::Get("multiply");
       auto out = tvm::relay::CallNode::make(op, inputs, tvm::Attrs(), {});
       return out;
     }},
    {Symbol::fromQualString("aten::avg_pool2d"),
     [](Node* node, tvm::Array<tvm::relay::Expr> inputs) {
       auto op = tvm::relay::Op::Get("nn.avg_pool2d");
       auto pool_attrs = tvm::make_node<tvm::relay::AvgPool2DAttrs>();
       pool_attrs->pool_size = relayToArray<tvm::relay::IndexExpr>(inputs[1]);
       auto strides = relayToArray<tvm::relay::IndexExpr>(inputs[2]);
       if (strides.size() == 0) {
         // pytorch avg_pool2d semantic: strides default to pool size
         pool_attrs->strides = pool_attrs->pool_size;
       } else {
         pool_attrs->strides = strides;
       }
       pool_attrs->padding = relayToArray<tvm::relay::IndexExpr>(inputs[3]);
       pool_attrs->layout = "NCHW";
       pool_attrs->ceil_mode = relayToConstant<bool>(inputs[4]);
       pool_attrs->count_include_pad = relayToConstant<bool>(inputs[5]);

       auto out = tvm::relay::CallNode::make(
           op, {inputs[0]}, tvm::Attrs(pool_attrs), {});
       return out;
     }},
    {Symbol::fromQualString("aten::adaptive_avg_pool2d"),
     [](Node* node, tvm::Array<tvm::relay::Expr> inputs) {
       static const tvm::relay::Op& op =
           tvm::relay::Op::Get("contrib.adaptive_avg_pool2d");
       auto pool_attrs = tvm::make_node<tvm::relay::AdaptivePool2DAttrs>();
       pool_attrs->output_size = relayToArray<tvm::relay::IndexExpr>(inputs[1]);
       pool_attrs->layout = "NCHW";
       auto out = tvm::relay::CallNode::make(
           op, {inputs[0]}, tvm::Attrs(pool_attrs), {});
       return out;
     }},
    {Symbol::fromQualString("aten::max_pool2d"),
     [](Node* node, tvm::Array<tvm::relay::Expr> inputs) {
       auto pool_attrs = tvm::make_node<tvm::relay::MaxPool2DAttrs>();
       pool_attrs->pool_size = relayToArray<tvm::relay::IndexExpr>(inputs[1]);
       auto strides = relayToArray<tvm::relay::IndexExpr>(inputs[2]);
       if (strides.size() == 0) {
         // pytorch max_pool2d semantic: strides default to pool size
         pool_attrs->strides = pool_attrs->pool_size;
       } else {
         pool_attrs->strides = strides;
       }
       pool_attrs->padding = relayToArray<tvm::relay::IndexExpr>(inputs[3]);
       pool_attrs->layout = "NCHW";
       // TODO: tvm has no dialtion but pytorch has, handle dilation
       pool_attrs->ceil_mode = relayToConstant<bool>(inputs[5]);

       static const tvm::relay::Op& op = tvm::relay::Op::Get("nn.max_pool2d");
       auto out = tvm::relay::CallNode::make(
           op, {inputs[0]}, tvm::Attrs(pool_attrs), {});
       return out;
     }},
    {Symbol::fromQualString("aten::reshape"),
     [](Node* node, tvm::Array<tvm::relay::Expr> inputs) {
       auto op = tvm::relay::Op::Get("reshape");
       auto attrs = tvm::make_node<tvm::relay::ReshapeAttrs>();
       attrs->newshape = relayToArray<tvm::Integer>(inputs[1]);
       TORCH_INTERNAL_ASSERT(attrs->newshape.size() > 0);
       if (static_cast<int64_t>(attrs->newshape[0]) != -1) {
	       std::cerr << "WARNING: reshape with -1 as the first value has known incompatibility with PyTorch semantics.\n";
       }
       attrs->reverse = false;
       auto out =
           tvm::relay::CallNode::make(op, {inputs[0]}, tvm::Attrs(attrs), {});
       return out;
     }},
    {Symbol::fromQualString("aten::linear"),
     [](Node* node, tvm::Array<tvm::relay::Expr> inputs) {
       Value* input = node->input(0);
       auto d_tensor = input->type()->cast<DimensionedTensorType>();
       if (d_tensor) {
         int64_t n_dim = d_tensor->dim();
         TORCH_CHECK(n_dim == 2,
                     "WARNING: relay does not support dense operation on inputs more than 2 dim");
       }
       auto dense_attrs = tvm::make_node<tvm::relay::DenseAttrs>();
       auto out = tvm::relay::CallNode::make(
           tvm::relay::Op::Get("nn.dense"),
           {inputs[0], inputs[1]},
           tvm::Attrs(dense_attrs),
           {});

       if (!relayIsNone(inputs[2])) {
         auto bias_add_op = tvm::relay::Op::Get("nn.bias_add");
         auto bias_add_attrs = tvm::make_node<tvm::relay::BiasAddAttrs>();
         bias_add_attrs->axis = 1;
         return tvm::relay::CallNode::make(
             bias_add_op, {out, inputs[2]}, tvm::Attrs(bias_add_attrs), {});
       }
       return out;
     }},
});

bool isSupported(Node* node) {
  auto map = getTVMOperatorMap();
  return map.find(node->kind()) != map.end();
}

tvm::relay::Expr getOperator(Node* node, tvm::Array<tvm::relay::Expr> inputs) {
  TORCH_INTERNAL_ASSERT(isSupported(node));
  return getTVMOperatorMap()[node->kind()](node, inputs);
}
