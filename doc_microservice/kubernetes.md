# ☸️ Kubernetes Deployment

> Deploy EventStreamCore on Kubernetes.

---

## 🎯 Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                    KUBERNETES DEPLOYMENT                             │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                      Ingress                                  │   │
│  │                  (eventstream.example.com)                    │   │
│  └───────────────────────────┬──────────────────────────────────┘   │
│                              │                                       │
│                              ▼                                       │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                    Service (ClusterIP)                        │   │
│  │           eventstream-headless (for StatefulSet)             │   │
│  └───────────────────────────┬──────────────────────────────────┘   │
│                              │                                       │
│         ┌────────────────────┼────────────────────┐                 │
│         │                    │                    │                 │
│         ▼                    ▼                    ▼                 │
│  ┌─────────────┐      ┌─────────────┐      ┌─────────────┐         │
│  │ Pod 0       │      │ Pod 1       │      │ Pod 2       │         │
│  │ eventstream │      │ eventstream │      │ eventstream │         │
│  │ -0          │      │ -1          │      │ -2          │         │
│  ├─────────────┤      ├─────────────┤      ├─────────────┤         │
│  │ PVC         │      │ PVC         │      │ PVC         │         │
│  │ data-0      │      │ data-1      │      │ data-2      │         │
│  └─────────────┘      └─────────────┘      └─────────────┘         │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                    ConfigMap                                  │   │
│  │              eventstream-config                               │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 📋 Manifests

### Namespace

```yaml
apiVersion: v1
kind: Namespace
metadata:
  name: eventstream
  labels:
    app: eventstream
```

### ConfigMap

```yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: eventstream-config
  namespace: eventstream
data:
  config.yaml: |
    server:
      tcp_port: 9000
      udp_port: 9001
      grpc_port: 9200
    
    queues:
      realtime_capacity: 16384
      batch_capacity: 65536
    
    cluster:
      enabled: true
      discovery: kubernetes
      namespace: eventstream
      service: eventstream-headless
    
    metrics:
      enabled: true
      port: 8080
      path: /metrics
```

### StatefulSet

```yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: eventstream
  namespace: eventstream
spec:
  serviceName: eventstream-headless
  replicas: 3
  selector:
    matchLabels:
      app: eventstream
  template:
    metadata:
      labels:
        app: eventstream
      annotations:
        prometheus.io/scrape: "true"
        prometheus.io/port: "8080"
    spec:
      affinity:
        podAntiAffinity:
          requiredDuringSchedulingIgnoredDuringExecution:
          - labelSelector:
              matchLabels:
                app: eventstream
            topologyKey: kubernetes.io/hostname
      
      containers:
      - name: eventstream
        image: eventstream/core:latest
        ports:
        - name: tcp
          containerPort: 9000
        - name: udp
          containerPort: 9001
          protocol: UDP
        - name: grpc
          containerPort: 9200
        - name: cluster
          containerPort: 9100
        - name: metrics
          containerPort: 8080
        
        env:
        - name: POD_NAME
          valueFrom:
            fieldRef:
              fieldPath: metadata.name
        - name: NODE_ID
          valueFrom:
            fieldRef:
              fieldPath: metadata.name
        
        volumeMounts:
        - name: config
          mountPath: /etc/eventstream
        - name: data
          mountPath: /var/lib/eventstream
        
        resources:
          requests:
            memory: "4Gi"
            cpu: "2"
          limits:
            memory: "8Gi"
            cpu: "4"
        
        readinessProbe:
          httpGet:
            path: /health
            port: 8080
          initialDelaySeconds: 5
          periodSeconds: 10
        
        livenessProbe:
          httpGet:
            path: /health
            port: 8080
          initialDelaySeconds: 15
          periodSeconds: 20
      
      volumes:
      - name: config
        configMap:
          name: eventstream-config
  
  volumeClaimTemplates:
  - metadata:
      name: data
    spec:
      accessModes: ["ReadWriteOnce"]
      storageClassName: fast-ssd
      resources:
        requests:
          storage: 100Gi
```

### Services

```yaml
# Headless service for StatefulSet
apiVersion: v1
kind: Service
metadata:
  name: eventstream-headless
  namespace: eventstream
spec:
  type: ClusterIP
  clusterIP: None
  selector:
    app: eventstream
  ports:
  - name: cluster
    port: 9100

---
# Client-facing service
apiVersion: v1
kind: Service
metadata:
  name: eventstream
  namespace: eventstream
spec:
  type: ClusterIP
  selector:
    app: eventstream
  ports:
  - name: tcp
    port: 9000
  - name: udp
    port: 9001
    protocol: UDP
  - name: grpc
    port: 9200
```

---

## 🚀 Helm Chart

### Install

```bash
# Add repo
helm repo add eventstream https://charts.eventstream.io
helm repo update

# Install
helm install eventstream eventstream/eventstream \
  --namespace eventstream \
  --create-namespace \
  --set replicas=3 \
  --set resources.requests.memory=4Gi \
  --set persistence.size=100Gi
```

### values.yaml

```yaml
# Cluster size
replicas: 3

# Image
image:
  repository: eventstream/core
  tag: latest
  pullPolicy: IfNotPresent

# Resources
resources:
  requests:
    memory: 4Gi
    cpu: 2
  limits:
    memory: 8Gi
    cpu: 4

# Persistence
persistence:
  enabled: true
  storageClass: fast-ssd
  size: 100Gi

# Ports
ports:
  tcp: 9000
  udp: 9001
  grpc: 9200
  cluster: 9100
  metrics: 8080

# NUMA optimization
numa:
  enabled: true
  
# Pod anti-affinity
affinity:
  podAntiAffinity: hard

# Metrics
metrics:
  enabled: true
  serviceMonitor:
    enabled: true
```

---

## 📈 Horizontal Pod Autoscaler

```yaml
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: eventstream
  namespace: eventstream
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: StatefulSet
    name: eventstream
  minReplicas: 3
  maxReplicas: 9
  metrics:
  - type: Resource
    resource:
      name: cpu
      target:
        type: Utilization
        averageUtilization: 70
  - type: Pods
    pods:
      metric:
        name: eventstream_queue_depth
      target:
        type: AverageValue
        averageValue: "50000"
```

---

## 🔄 Rolling Update

```
┌─────────────────────────────────────────────────────────────────────┐
│                    ROLLING UPDATE SEQUENCE                           │
│                                                                      │
│  StatefulSet updates pods in reverse order (N-1 → 0)                │
│                                                                      │
│  Step 1: Update Pod 2                                               │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  Pod 0: v1.0 (Leader)    ✓ Running                           │   │
│  │  Pod 1: v1.0             ✓ Running                           │   │
│  │  Pod 2: v1.0 → v1.1      ⟳ Updating                          │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  Step 2: Update Pod 1                                               │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  Pod 0: v1.0 (Leader)    ✓ Running                           │   │
│  │  Pod 1: v1.0 → v1.1      ⟳ Updating                          │   │
│  │  Pod 2: v1.1             ✓ Running                           │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  Step 3: Update Pod 0 (Leader)                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  Pod 0: v1.0 → v1.1      ⟳ Updating (triggers election)     │   │
│  │  Pod 1: v1.1 (New Leader) ✓ Running                          │   │
│  │  Pod 2: v1.1             ✓ Running                           │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  Complete:                                                           │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  Pod 0: v1.1             ✓ Running                           │   │
│  │  Pod 1: v1.1 (Leader)    ✓ Running                           │   │
│  │  Pod 2: v1.1             ✓ Running                           │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## ➡️ Next

- [Monitoring →](monitoring.md)
