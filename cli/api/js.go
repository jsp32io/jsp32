package api

// JS provides access to the JavaScript API group.
type JS struct {
	client *Client
}

// JS returns the JavaScript API group for this client.
func (c *Client) JS() *JS {
	return &JS{client: c}
}
