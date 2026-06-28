#ifndef CAKESPARK_H
#define CAKESPARK_H

#ifdef __cplusplus
extern "C" {
#endif

#define CAKE_OK       0
#define CAKE_RUNNING  1
#define CAKE_ERROR   -1

typedef struct CakeVM     CakeVM;
typedef void*             CakeValue;

typedef struct {
  int maxCallDepth;
  int maxIterations;
  int maxTicks;
  int maxVariables;
  int maxMemory;
} CakeLimits;

typedef CakeValue (*CakeFn)(CakeVM* vm, int argc, CakeValue* argv);
typedef CakeValue (*CakeGetter)(CakeVM* vm);
typedef void      (*CakeSetter)(CakeVM* vm, CakeValue val);

/* setter == NULL marks the property read-only */
typedef struct {
  const char* name;
  CakeGetter  getter;
  CakeSetter  setter;
} CakePeripheralProperty;

typedef struct {
  const char* name;
  CakeFn      handler;
} CakePeripheralMethod;

typedef struct {
  const CakePeripheralProperty* properties;
  int                           propertyCount;
  const CakePeripheralMethod*   methods;
  int                           methodCount;
} CakePeripheral;

const char* cake_version(void);

CakeVM*  cake_new(CakeLimits limits);
void     cake_free(CakeVM* vm);

int      cake_compile(CakeVM* vm, const char* source);
int      cake_tick(CakeVM* vm);
int      cake_run(CakeVM* vm);

void     cake_register_fn(CakeVM* vm, const char* name, CakeFn handler);
void     cake_register_peripheral(CakeVM* vm, const char* name, CakePeripheral* p);

CakeValue cake_get_var(CakeVM* vm, const char* name);
void      cake_set_var(CakeVM* vm, const char* name, CakeValue val);

const char* cake_get_output(CakeVM* vm);
const char* cake_get_error(CakeVM* vm);

const char* cake_save_state(CakeVM* vm);
int         cake_load_state(CakeVM* vm, const char* json);

#ifdef __cplusplus
}
#endif

#endif /* CAKESPARK_H */
