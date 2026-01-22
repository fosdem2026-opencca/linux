#include <linux/clk.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#define GPU_ID 0x0
#define GPU_SHADER_PRESENT_LO 0x100

struct gpu_poweron_dev
{
    void __iomem* iomem;
    struct clk* clk;
};

// Setup code from panthor driver
// For Demo, we bring up power on GPU such that MMIO is accessible
// The rest is handled in the CVM directly

static int
gpu_poweron_probe(struct platform_device* pdev)
{
    struct gpu_poweron_dev* gpdev;
    u32 gpu_id, shaders;
    int ret;

    gpdev = devm_kzalloc(&pdev->dev, sizeof(*gpdev), GFP_KERNEL);
    if (!gpdev)
        return -ENOMEM;

    platform_set_drvdata(pdev, gpdev);

    gpdev->iomem = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(gpdev->iomem))
        return PTR_ERR(gpdev->iomem);

    gpdev->clk = devm_clk_get(&pdev->dev, NULL);
    if (IS_ERR(gpdev->clk))
        return PTR_ERR(gpdev->clk);

    ret = devm_pm_runtime_enable(&pdev->dev);
    if (ret)
        return ret;

    ret = pm_runtime_resume_and_get(&pdev->dev);
    if (ret)
        return ret;

    ret = clk_prepare_enable(gpdev->clk);
    if (ret) {
        pm_runtime_put_sync(&pdev->dev);
        return ret;
    }

    gpu_id = readl(gpdev->iomem + GPU_ID);
    shaders = readl(gpdev->iomem + GPU_SHADER_PRESENT_LO);
    dev_info(&pdev->dev, "GPU ID: 0x%08x, shaders: 0x%x\n", gpu_id, shaders);

    return 0;
}

static void
gpu_poweron_remove(struct platform_device* pdev)
{
    struct gpu_poweron_dev* gpdev = platform_get_drvdata(pdev);

    clk_disable_unprepare(gpdev->clk);
    pm_runtime_put_sync(&pdev->dev);
}

static const struct of_device_id gpu_poweron_of_match[] = {
    { .compatible = "rockchip,rk3588-mali" },
    {}
};
MODULE_DEVICE_TABLE(of, gpu_poweron_of_match);

static struct platform_driver gpu_poweron_driver = {
	.probe = gpu_poweron_probe,
	.remove = gpu_poweron_remove,
	.driver = {
		.name = "gpu_poweron",
		.of_match_table = gpu_poweron_of_match,
	},
};

module_platform_driver(gpu_poweron_driver);
MODULE_LICENSE("GPL");
