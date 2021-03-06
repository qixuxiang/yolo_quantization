#include "route_layer.h"
#include "cuda.h"
#include "blas.h"

#include <stdio.h>

route_layer make_route_layer(int batch, int n, int *input_layers, int *input_sizes, int layer_quant_flag, int quant_stop_flag, int close_quantization)
{
    fprintf(stderr,"route ");
    route_layer l = {0};
    l.type = ROUTE;
    l.batch = batch;
    l.n = n;
    l.input_layers = input_layers;
    l.input_sizes = input_sizes;
    int i;
    int outputs = 0;
    for(i = 0; i < n; ++i){
        fprintf(stderr," %d", input_layers[i]);
        outputs += input_sizes[i];
    }
    printf("\n");
    l.outputs = outputs;
    l.inputs = outputs;
    l.close_quantization = close_quantization;
    l.delta =  calloc(outputs*batch, sizeof(float));
    l.output = calloc(outputs*batch, sizeof(float));
    l.forward = forward_route_layer;
#ifdef QUANTIZATION
	l.activ_data_uint8_scales = calloc(1, sizeof(float));
    l.activ_data_uint8_zero_point = calloc(1, sizeof(uint8_t));
    l.min_activ_value = calloc(1, sizeof(float));
    l.max_activ_value = calloc(1, sizeof(float));
    l.output_uint8_final = calloc(l.batch*l.outputs, sizeof(uint8_t));

    l.layer_quant_flag = layer_quant_flag;
    l.quant_stop_flag = quant_stop_flag;
    if(l.layer_quant_flag && !l.close_quantization){
    // if(l.layer_quant_flag && !l.close_quantization && l.n == 1){
        l.forward = forward_route_layer_quant;
        // l.forward = forward_route_layer;
    }
    else{
        l.forward = forward_route_layer;
    }
#endif
    l.backward = backward_route_layer;
    #ifdef GPU
    l.forward_gpu = forward_route_layer_gpu;
    l.backward_gpu = backward_route_layer_gpu;

    l.delta_gpu =  cuda_make_array(l.delta, outputs*batch);
    l.output_gpu = cuda_make_array(l.output, outputs*batch);
    #endif
    return l;
}

void resize_route_layer(route_layer *l, network *net)
{
    int i;
    layer first = net->layers[l->input_layers[0]];
    l->out_w = first.out_w;
    l->out_h = first.out_h;
    l->out_c = first.out_c;
    l->outputs = first.outputs;
    l->input_sizes[0] = first.outputs;
    for(i = 1; i < l->n; ++i){
        int index = l->input_layers[i];
        layer next = net->layers[index];
        l->outputs += next.outputs;
        l->input_sizes[i] = next.outputs;
        if(next.out_w == first.out_w && next.out_h == first.out_h){
            l->out_c += next.out_c;
        }else{
            printf("%d %d, %d %d\n", next.out_w, next.out_h, first.out_w, first.out_h);
            l->out_h = l->out_w = l->out_c = 0;
        }
    }
    l->inputs = l->outputs;
    l->delta =  realloc(l->delta, l->outputs*l->batch*sizeof(float));
    l->output = realloc(l->output, l->outputs*l->batch*sizeof(float));

#ifdef GPU
    cuda_free(l->output_gpu);
    cuda_free(l->delta_gpu);
    l->output_gpu  = cuda_make_array(l->output, l->outputs*l->batch);
    l->delta_gpu   = cuda_make_array(l->delta,  l->outputs*l->batch);
#endif
    
}

void forward_route_layer(const route_layer l, network net)
{
    int i, j;
    int offset = 0;
    for(i = 0; i < l.n; ++i){
        int index = l.input_layers[i];
        float *input = net.layers[index].output;
        int input_size = l.input_sizes[i];
        for(j = 0; j < l.batch; ++j){
            copy_cpu(input_size, input + j*input_size, 1, l.output + offset + j*l.outputs, 1);
        }
        offset += input_size;
    }
}

void forward_route_layer_quant(const route_layer l, network net)
{
    int i, j;
    int offset = 0;
    for(i = 0; i < l.n; ++i){
        int index = l.input_layers[i];
        uint8_t *input_uint8 = net.layers[index].output_uint8_final;
        int input_size = l.input_sizes[i];
        for(j = 0; j < l.batch; ++j){
            copy_cpu_uint8(input_size, input_uint8 + j*input_size, 1, l.output_uint8_final + offset + j*l.outputs, 1);
        }
        if(l.quant_stop_flag){
            // printf("dequant from uint8 to float32 in layer %d\n", l.count);
            #pragma omp parallel for
            for (int s = 0; s < net.layers[index].out_c; ++s) {
                for (int t = 0; t < net.layers[index].out_w*net.layers[index].out_h; ++t){
                    int out_index = s*net.layers[index].out_w*net.layers[index].out_h + t + offset ;
                    l.output[out_index] = (l.output_uint8_final[out_index] -  net.layers[index].activ_data_uint8_zero_point[0]) * net.layers[index].activ_data_uint8_scales[0];
                }
            }
        }
        offset += input_size; 
    }
}

void backward_route_layer(const route_layer l, network net)
{
    int i, j;
    int offset = 0;
    for(i = 0; i < l.n; ++i){
        int index = l.input_layers[i];
        float *delta = net.layers[index].delta;
        int input_size = l.input_sizes[i];
        for(j = 0; j < l.batch; ++j){
            axpy_cpu(input_size, 1, l.delta + offset + j*l.outputs, 1, delta + j*input_size, 1);
        }
        offset += input_size;
    }
}

#ifdef GPU
void forward_route_layer_gpu(const route_layer l, network net)
{
    int i, j;
    int offset = 0;
    for(i = 0; i < l.n; ++i){
        int index = l.input_layers[i];
        float *input = net.layers[index].output_gpu;
        int input_size = l.input_sizes[i];
        for(j = 0; j < l.batch; ++j){
            copy_gpu(input_size, input + j*input_size, 1, l.output_gpu + offset + j*l.outputs, 1);
        }
        offset += input_size;
    }
    int step = *net.seen;
    int quant_step = 10000;
    if(net.train && l.layer_quant_flag && step > quant_step && l.n > 1){
    // if(net.train && l.layer_quant_flag && l.n > 1){
        cuda_pull_array(l.output_gpu, l.output, l.out_c*l.out_w*l.out_h);
        uint8_t input_fake_quant = 0;
        fake_quant_with_min_max_channel(1, l.output, &input_fake_quant, l.out_c*l.out_w*l.out_h, l.min_activ_value, l.max_activ_value, 
                                        l.activ_data_uint8_scales, l.activ_data_uint8_zero_point, ACTIV_QUANT, 0.999);
        assert(l.activ_data_uint8_scales[0] > 0);
        cuda_push_array(l.output_gpu, l.output, l.out_c*l.out_w*l.out_h);
    }
}

void backward_route_layer_gpu(const route_layer l, network net)
{
    // cuda_pull_array(net.delta_gpu, net.delta, l.input_sizes[0]);
    // int num = 0, num_l = 0;
    // for(int ii = 0; ii < l.input_sizes[0]; ++ii){
    //     if (net.delta[ii] != 0){
    //         num++;
    //         net.delta[ii] = 0;
    //     }
    // }
    // printf("%d net.delta = %d, l.delta = %d\n", l.count, num, num_l);
    int i, j;
    int offset = 0;
    for(i = 0; i < l.n; ++i){
        int index = l.input_layers[i];
        float *delta = net.layers[index].delta_gpu;
        int input_size = l.input_sizes[i];
        for(j = 0; j < l.batch; ++j){
            axpy_gpu(input_size, 1, l.delta_gpu + offset + j*l.outputs, 1, delta + j*input_size, 1);
        }
        // cuda_pull_array(net.layers[index].delta_gpu, net.layers[index].delta, input_size);
        // printf("%d route net.delta = %f\n", net.layers[index].count, net.layers[index].delta[100]);
        offset += input_size;
    }
}
#endif
